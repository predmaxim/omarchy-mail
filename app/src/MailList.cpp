#include "MailList.h"
#include "Format.h"
#include "Highlight.h"
#include "I18n.h"

#include <QDateTime>
#include <algorithm>

namespace om {

MailList::MailList(QObject *parent) : QAbstractListModel(parent) {}

int MailList::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(items_.size()); }

QHash<int, QByteArray> MailList::roleNames() const
{
    return {{KeyRole, "key"}, {IdRole, "id"}, {SenderRole, "sender"}, {SubjectRole, "subject"},
            {PreviewRole, "preview"}, {SubjectRichRole, "subjectRich"}, {PreviewRichRole, "previewRich"},
            {DateTextRole, "dateText"}, {PlaceRole, "place"}, {SeenRole, "seen"}, {FlaggedRole, "flagged"},
            {AttachmentsRole, "attachments"}, {QueuedRole, "queued"}, {FailedRole, "failed"},
            {RemoteRole, "remote"}, {SelectedRole, "selected"}, {IsCursorRole, "isCursor"},
            {GroupRole, "group"}};
}

QString MailList::keyOf(const QJsonObject &j)
{
    const qint64 id = j.value("id").toInteger();
    if (id != 0)
        return QStringLiteral("m%1").arg(id);
    const QJsonObject r = j.value("remote").toObject();
    return QStringLiteral("r%1/%2/%3").arg(r.value("account").toVariant().toString(),
                                           r.value("folder_path").toString(),
                                           r.value("uid").toVariant().toString());
}

MailList::Item MailList::makeItem(const QJsonObject &j, bool remote) const
{
    Item it;
    it.json = j;
    it.key = keyOf(j);
    it.remote = remote || (j.value("id").toInteger() == 0 && j.contains("remote"));
    it.subjectRich = highlight::styled(j.value("subject").toString(), words_, mark_);
    it.previewRich = highlight::styled(j.value("preview").toString(), words_, mark_);
    if (place_)
        it.place = place_(j);
    return it;
}

QVariant MailList::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= items_.size())
        return {};
    const Item &it = items_.at(index.row());
    const QJsonObject &j = it.json;
    switch (role) {
    case KeyRole: return it.key;
    case IdRole: return j.value("id").toInteger();
    case SenderRole: {
        const QJsonObject f = j.value("from").toObject();
        const QString n = f.value("name").toString();
        return n.isEmpty() ? f.value("addr").toString() : n;
    }
    case SubjectRole: return j.value("subject").toString();
    case PreviewRole: return j.value("preview").toString();
    case SubjectRichRole: return it.subjectRich;
    case PreviewRichRole: return it.previewRich;
    case DateTextRole:
        return format::listDate(j.value("date").toInteger(), QDateTime::currentDateTime(), timeLocale());
    case PlaceRole: return it.place;
    case SeenRole: return j.value("seen").toBool();
    case FlaggedRole: return j.value("flagged").toBool();
    case AttachmentsRole: return j.value("has_attachments").toBool();
    case QueuedRole: return j.value("queued").toBool();
    case FailedRole: return j.value("failed").toBool();
    case RemoteRole: return j.contains("remote");
    case SelectedRole: return selected_.contains(it.key);
    case IsCursorRole: return index.row() == cursor_;
    case GroupRole: return it.remote ? QStringLiteral("remote") : QStringLiteral("local");
    }
    return {};
}

void MailList::touch(const QList<int> &roles)
{
    if (!items_.isEmpty())
        Q_EMIT dataChanged(index(0), index(int(items_.size()) - 1), roles);
}

void MailList::setSelection(const QSet<QString> &s)
{
    if (s == selected_)
        return;
    int lo = -1, hi = -1;
    for (int i = 0; i < items_.size(); ++i)
        if (s.contains(items_.at(i).key) != selected_.contains(items_.at(i).key)) {
            if (lo < 0)
                lo = i;
            hi = i;
        }
    selected_ = s;
    if (lo >= 0)
        Q_EMIT dataChanged(index(lo), index(hi), {SelectedRole});
    Q_EMIT selectionChanged();
}

void MailList::setCursorOnly(int i)
{
    i = items_.isEmpty() ? -1 : std::clamp(i, 0, int(items_.size()) - 1);
    if (i == cursor_)
        return;
    const int old = cursor_;
    cursor_ = i;
    for (int r : {old, cursor_})
        if (r >= 0)
            Q_EMIT dataChanged(index(r), index(r), {IsCursorRole});
    Q_EMIT cursorChanged();
}

void MailList::setCursor(int i) { setCursorOnly(i); }

void MailList::setRows(const QJsonArray &summaries)
{
    const QString oldKey = cursor_ >= 0 ? items_.at(cursor_).key : QString();
    const int oldCursor = cursor_;
    beginResetModel();
    items_.clear();
    for (const QJsonValue &v : summaries)
        items_.append(makeItem(v.toObject(), false));
    QSet<QString> keys;
    for (const Item &it : std::as_const(items_))
        keys.insert(it.key);
    selected_.intersect(keys);
    base_.intersect(keys);
    anchor_ = -1;
    int c = -1;
    for (int i = 0; i < items_.size() && !oldKey.isEmpty(); ++i)
        if (items_.at(i).key == oldKey) { c = i; break; }
    if (c < 0)
        c = items_.isEmpty() ? -1 : std::clamp(oldCursor, 0, int(items_.size()) - 1);
    cursor_ = c;
    endResetModel();
    Q_EMIT countChanged();
    const bool same = c >= 0 && items_.at(c).key == oldKey;
    Q_EMIT rowsReplaced(same);
    if (c != oldCursor || (c >= 0 && !same))   // a reload of the same rows moves nothing
        Q_EMIT cursorChanged();
    Q_EMIT selectionChanged();
}

void MailList::addItems(const QJsonArray &a, bool remote)
{
    QSet<QString> have;
    for (const Item &it : std::as_const(items_))
        have.insert(it.key);
    QList<Item> add;
    for (const QJsonValue &v : a) {
        Item it = makeItem(v.toObject(), remote);
        if (have.contains(it.key))
            continue;
        have.insert(it.key);
        add.append(it);
    }
    if (add.isEmpty())
        return;
    beginInsertRows({}, int(items_.size()), int(items_.size() + add.size()) - 1);
    items_ += add;
    endInsertRows();
    Q_EMIT countChanged();
    if (cursor_ < 0)
        setCursorOnly(0);
}

void MailList::appendRows(const QJsonArray &summaries) { addItems(summaries, false); }
void MailList::appendRemote(const QJsonArray &hits) { addItems(hits, true); }

void MailList::setWords(const QStringList &words, const QColor &mark)
{
    words_ = words;
    mark_ = mark;
    for (Item &it : items_) {
        it.subjectRich = highlight::styled(it.json.value("subject").toString(), words_, mark_);
        it.previewRich = highlight::styled(it.json.value("preview").toString(), words_, mark_);
    }
    touch({SubjectRichRole, PreviewRichRole});
}

void MailList::setPlace(std::function<QString(const QJsonObject &)> place)
{
    place_ = std::move(place);
    for (Item &it : items_)
        it.place = place_ ? place_(it.json) : QString();
    touch({PlaceRole});
}

QJsonObject MailList::row(int i) const
{
    return i >= 0 && i < items_.size() ? items_.at(i).json : QJsonObject();
}

int MailList::initialCursor() const
{
    if (items_.isEmpty())
        return -1;
    for (int i = 0; i < items_.size(); ++i) {
        const QJsonObject &j = items_.at(i).json;
        if (!j.value("seen").toBool() && j.value("id").toInteger() > 0)
            return i;
    }
    return 0;
}

// selection = base_ ∪ range(anchor_..to); cursor follows
void MailList::selectRange(int to)
{
    QSet<QString> s = base_;
    for (int i = std::min(anchor_, to); i <= std::max(anchor_, to); ++i)
        s.insert(items_.at(i).key);
    setSelection(s);
}

void MailList::move(int dir, bool ctrl, bool shift)
{
    if (items_.isEmpty())
        return;
    const int from = std::max(cursor_, 0);
    const int to = std::clamp(from + dir, 0, int(items_.size()) - 1);
    if (shift) {
        if (anchor_ < 0) {
            anchor_ = from;
            base_ = selected_;
        }
        setCursorOnly(to);
        selectRange(to);
    } else {
        setCursorOnly(to);
        if (!ctrl) {
            anchor_ = -1;
            base_.clear();
            setSelection({});
        }
    }
}

void MailList::click(int index, bool ctrl, bool shift)
{
    if (index < 0 || index >= items_.size())
        return;
    if (shift) {
        if (anchor_ < 0) {
            anchor_ = std::max(cursor_, 0);
            base_ = selected_;
        }
        setCursorOnly(index);
        selectRange(index);
    } else if (ctrl) {
        setCursorOnly(index);
        toggleMark();
    } else {
        setCursorOnly(index);
        anchor_ = index;
        base_.clear();
        setSelection({});
    }
}

void MailList::toggleMark()
{
    if (cursor_ < 0)
        return;
    QSet<QString> s = selected_;
    const QString &k = items_.at(cursor_).key;
    if (!s.remove(k))
        s.insert(k);
    setSelection(s);
    anchor_ = cursor_;
    base_ = selected_;
}

void MailList::selectAll()
{
    QSet<QString> s;
    for (const Item &it : std::as_const(items_))
        s.insert(it.key);
    setSelection(s);
    anchor_ = -1;
}

void MailList::clearSelection()
{
    anchor_ = -1;
    base_.clear();
    setSelection({});
}

QList<qint64> MailList::targetIds() const
{
    QList<qint64> out;
    if (selected_.isEmpty()) {
        const qint64 id = cursorRow().value("id").toInteger();
        if (cursor_ >= 0 && id > 0)
            out.append(id);
        return out;
    }
    for (const Item &it : items_) {
        const qint64 id = it.json.value("id").toInteger();
        if (id > 0 && selected_.contains(it.key))
            out.append(id);
    }
    return out;
}

void MailList::removeIds(const QList<qint64> &ids)
{
    const QSet<qint64> gone(ids.begin(), ids.end());
    QList<int> idx;
    for (int i = 0; i < items_.size(); ++i) {
        const qint64 id = items_.at(i).json.value("id").toInteger();
        if (id != 0 && gone.contains(id))
            idx.append(i);
    }
    if (idx.isEmpty())
        return;
    const bool hadSel = !selected_.isEmpty();
    int before = 0;
    for (int i = 0; i < cursor_; ++i)
        before += !idx.contains(i);
    selected_.clear();
    base_.clear();
    anchor_ = -1;
    touch({SelectedRole});
    // contiguous runs, from the end so earlier indices stay valid
    for (int e = int(idx.size()) - 1; e >= 0;) {
        int s = e;
        while (s > 0 && idx.at(s - 1) == idx.at(s) - 1)
            --s;
        beginRemoveRows({}, idx.at(s), idx.at(e));
        items_.remove(idx.at(s), idx.at(e) - idx.at(s) + 1);
        endRemoveRows();
        e = s - 1;
    }
    cursor_ = items_.isEmpty() ? -1 : std::min(before, int(items_.size()) - 1);
    if (cursor_ >= 0)
        Q_EMIT dataChanged(index(cursor_), index(cursor_), {IsCursorRole});
    Q_EMIT countChanged();
    Q_EMIT cursorChanged();
    if (hadSel)
        Q_EMIT selectionChanged();
}

void MailList::setFlags(const QList<qint64> &ids, std::optional<bool> seen, std::optional<bool> flagged)
{
    const QSet<qint64> want(ids.begin(), ids.end());
    for (int i = 0; i < items_.size(); ++i) {
        const qint64 id = items_.at(i).json.value("id").toInteger();
        if (id == 0 || !want.contains(id))
            continue;
        if (seen)
            items_[i].json.insert("seen", *seen);
        if (flagged)
            items_[i].json.insert("flagged", *flagged);
        Q_EMIT dataChanged(index(i), index(i), {SeenRole, FlaggedRole});
    }
}

} // namespace om
