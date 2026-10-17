#include "Compose.h"
#include "Format.h"
#include "I18n.h"
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>

namespace om {

namespace {
const QRegularExpression &addrRe()
{
    static const QRegularExpression re(QStringLiteral("^[^@\\s<>]+@[^@\\s<>]+\\.[^@\\s<>]+$"));
    return re;
}

void privateDir(const QString &path)
{
    QDir().mkpath(path);
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}
} // namespace

MailtoDraft parseMailto(const QString &mailto)
{
    const QUrl url(mailto);
    const QUrlQuery q(url);
    return {url.path(QUrl::FullyDecoded), q.queryItemValue(QStringLiteral("subject"), QUrl::FullyDecoded),
            q.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded)};
}

QString Compose::defaultDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/omarchy-mail/drafts");
}

Compose::Compose(QString draftsDir, QObject *parent) : QObject(parent), m_dir(std::move(draftsDir))
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1000);
    connect(&m_saveTimer, &QTimer::timeout, this, &Compose::save);
}

QVariantList Compose::attachments() const
{
    QVariantList out;
    for (const Att &a : m_atts) {
        const QFileInfo fi(a.path);
        const bool img = QStringList{"png", "jpg", "jpeg", "gif", "webp"}.contains(fi.suffix().toLower());
        out << QVariantMap{{"name", fi.fileName()}, {"sizeText", format::size(fi.size())},
                           {"icon", img ? QStringLiteral("image") : QStringLiteral("file")}};
    }
    return out;
}

void Compose::setSuggestions(const QVariantList &s)
{
    m_suggestions = s;
    Q_EMIT changed();
}

void Compose::edit(QString &field, const QString &v)
{
    if (field == v)
        return;
    field = v;
    m_saveTimer.start();
    Q_EMIT changed();
}

void Compose::setNote(const QString &text, bool error)
{
    m_note = text;
    m_noteError = error;
    Q_EMIT changed();
}

void Compose::begin(const QString &key, const QString &account)
{
    if (m_open)
        save();
    m_key = key;
    m_replyId = 0;
    m_account = account;
    m_to.clear();
    m_subject.clear();
    m_body.clear();
    m_note.clear();
    m_noteError = false;
    m_cursor = 0;
    m_atts.clear();
    m_suggestions.clear();
    m_confirm = {};
    m_open = true;
}

void Compose::load()
{
    QFile f(draftPath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject j = QJsonDocument::fromJson(f.readAll()).object();
    m_account = j.value("account").toString(m_account);
    m_to = j.value("to").toString();
    m_subject = j.value("subject").toString();
    m_body = j.value("body").toString();
    m_cursor = j.value("cursor").toInt();
    for (const QJsonValue &v : j.value("attachments").toArray()) {
        const QJsonObject a = v.toObject();
        if (QFileInfo::exists(a.value("path").toString()))
            m_atts.append({a.value("path").toString(), a.value("pasted").toBool()});
    }
}

bool Compose::save()
{
    m_saveTimer.stop();
    if (!m_open)   // late edits after sent()/discard()/close() must not resurrect a draft
        return true;
    privateDir(m_dir);
    QJsonArray atts;
    for (const Att &a : m_atts)
        atts.append(QJsonObject{{"path", a.path}, {"pasted", a.pasted}});
    const QJsonObject j{{"account", m_account}, {"to", m_to}, {"subject", m_subject}, {"body", m_body},
                        {"cursor", m_cursor}, {"attachments", atts}};
    QSaveFile f(draftPath());
    f.setDirectWriteFallback(false);
    if (f.open(QIODevice::WriteOnly)) {
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (f.write(QJsonDocument(j).toJson(QJsonDocument::Compact)) >= 0 && f.commit())
            return true;
    }
    setNote(t("Could not save draft"), true);
    return false;
}

void Compose::removeDraft()
{
    m_saveTimer.stop();
    for (const Att &a : m_atts)
        if (a.pasted)
            QFile::remove(a.path);
    m_atts.clear();
    QFile::remove(draftPath());
    m_open = false;
    Q_EMIT changed();
}

void Compose::startNew(const QString &account)
{
    begin(QStringLiteral("new"), account);
    load();
    Q_EMIT changed();
}

void Compose::startReply(const QJsonObject &full)
{
    const QJsonObject s = full.value("summary").toObject();
    begin(QStringLiteral("reply-%1").arg(s.value("id").toInteger()), s.value("account").toString());
    m_replyId = s.value("id").toInteger();
    if (QFileInfo::exists(draftPath())) {
        load();
    } else {
        const QJsonObject from = s.value("from").toObject();
        const QString name = from.value("name").toString();
        const QString addr = from.value("addr").toString();
        m_to = completeTo({}, name, addr).chopped(2);
        m_subject = replySubject(s.value("subject").toString());
        m_body = QStringLiteral("\n\n") + quote(full, timeLocale());
        m_cursor = 0;
    }
    Q_EMIT changed();
}

void Compose::startMailto(const QUrl &url, const QString &account)
{
    startNew(account);
    const MailtoDraft d = parseMailto(url.toString());
    QString bad;
    bool have = false;
    for (const auto &p : parseAddresses(m_to, &bad))
        have = have || p.second.compare(d.to, Qt::CaseInsensitive) == 0;
    if (!d.to.isEmpty() && !have)
        m_to = m_to.trimmed().isEmpty() ? d.to : m_to.trimmed() + QStringLiteral(", ") + d.to;
    if (m_subject.isEmpty())
        m_subject = d.subject;
    if (m_body.isEmpty())
        m_body = d.body;
    m_saveTimer.start();
    Q_EMIT changed();
}

void Compose::close()
{
    if (!m_open || !save())
        return;
    m_open = false;
    Q_EMIT changed();
}

void Compose::discard()
{
    if (!m_confirm.press(QStringLiteral("discard"), QDateTime::currentMSecsSinceEpoch())) {
        setNote(t("Press Ctrl+D again to discard"));
        return;
    }
    removeDraft();
}

void Compose::addFiles(const QList<QUrl> &urls)
{
    bool added = false;
    for (const QUrl &u : urls) {
        const QFileInfo fi(u.toLocalFile());
        if (!u.isLocalFile() || !fi.isFile())
            continue;
        m_atts.append({fi.absoluteFilePath(), false});
        added = true;
    }
    if (added) {
        m_saveTimer.start();
        Q_EMIT changed();
    }
}

bool Compose::paste()
{
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (!md)
        return false;
    if (md->hasImage()) {
        const QImage img = qvariant_cast<QImage>(md->imageData());
        if (img.isNull())
            return false;
        const QString filesDir = m_dir + QStringLiteral("/files");
        privateDir(m_dir);
        privateDir(filesDir);
        const QString base = QStringLiteral("%1/%2-%3").arg(
            filesDir, t("screenshot"), QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd-HH-mm")));
        QString path = base + QStringLiteral(".png");
        for (int n = 2; QFileInfo::exists(path); ++n)
            path = QStringLiteral("%1-%2.png").arg(base).arg(n);
        if (!img.save(path, "PNG"))
            return false;
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        m_atts.append({path, true});
        m_saveTimer.start();
        Q_EMIT changed();
        return true;
    }
    if (md->hasUrls()) {
        const int before = m_atts.size();
        addFiles(md->urls());
        return m_atts.size() > before;
    }
    return false;
}

void Compose::removeAttachment(int i)
{
    if (i < 0 || i >= m_atts.size())
        return;
    if (m_atts.at(i).pasted)
        QFile::remove(m_atts.at(i).path);
    m_atts.removeAt(i);
    m_saveTimer.start();
    Q_EMIT changed();
}

void Compose::pickSuggestion(int i)
{
    if (i < 0 || i >= m_suggestions.size())
        return;
    const QVariantMap s = m_suggestions.at(i).toMap();
    m_suggestions.clear();
    setTo(completeTo(m_to, s.value("name").toString(), s.value("addr").toString()));
    Q_EMIT changed();
}

QJsonObject Compose::request(QString *error) const
{
    QString bad;
    const auto addrs = parseAddresses(m_to, &bad);
    if (!bad.isEmpty()) {
        *error = t("Invalid address: %1").arg(bad);
        return {};
    }
    if (addrs.isEmpty()) {
        *error = t("Add a recipient");
        return {};
    }
    QJsonArray to, atts;
    for (const auto &p : addrs)
        to.append(QJsonObject{{"name", p.first}, {"addr", p.second}});
    for (const Att &a : m_atts)
        atts.append(a.path);
    QJsonObject r{{"cmd", "send"}, {"account", m_account}, {"to", to}, {"subject", m_subject},
                  {"text", m_body}, {"attachments", atts}};
    if (m_replyId)
        r.insert("reply_to", m_replyId);
    return r;
}

Compose::Draft Compose::draft() const
{
    Draft d{m_key, {}};
    for (const Att &a : m_atts)
        if (a.pasted)
            d.pasted << a.path;
    return d;
}

void Compose::sent(const Draft &d)
{
    if (shows(d)) {
        removeDraft();
        return;
    }
    for (const QString &p : d.pasted)   // the form moved on: only the sent draft goes, the open one stays
        QFile::remove(p);
    QFile::remove(draftPath(d.key));
}

QList<QPair<QString, QString>> Compose::parseAddresses(const QString &field, QString *bad)
{
    QStringList parts;
    QString cur;
    bool quoted = false, angle = false;
    for (const QChar c : field) {
        if (c == QLatin1Char('"'))
            quoted = !quoted;
        else if (!quoted && c == QLatin1Char('<'))
            angle = true;
        else if (!quoted && c == QLatin1Char('>'))
            angle = false;
        if (!quoted && !angle && (c == QLatin1Char(',') || c == QLatin1Char(';'))) {
            parts << cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts << cur;
    QList<QPair<QString, QString>> out;
    for (const QString &raw : parts) {
        const QString p = raw.trimmed();
        if (p.isEmpty())
            continue;
        QString name, addr = p;
        const int lt = p.lastIndexOf(QLatin1Char('<'));
        if (lt >= 0 && p.endsWith(QLatin1Char('>'))) {
            name = p.left(lt).trimmed();
            addr = p.mid(lt + 1, p.size() - lt - 2).trimmed();
            if (name.size() >= 2 && name.startsWith(QLatin1Char('"')) && name.endsWith(QLatin1Char('"')))
                name = name.mid(1, name.size() - 2);
        }
        if (!addrRe().match(addr).hasMatch()) {
            if (bad && bad->isEmpty())
                *bad = p;
            continue;
        }
        out.append({name, addr});
    }
    return out;
}

QString Compose::replySubject(const QString &subject)
{
    static const QRegularExpression re(QStringLiteral("^\\s*re:"), QRegularExpression::CaseInsensitiveOption);
    return re.match(subject).hasMatch() ? subject : QStringLiteral("Re: ") + subject;
}

QString Compose::quote(const QJsonObject &full, const QLocale &time)
{
    const QJsonObject s = full.value("summary").toObject();
    const QJsonObject from = s.value("from").toObject();
    const QString who = from.value("name").toString().isEmpty() ? from.value("addr").toString()
                                                                : from.value("name").toString();
    QStringList lines{t("%1, %2 wrote:").arg(format::quoteDate(s.value("date").toInteger(), time), who)};
    for (const QString &l : full.value("text").toString().split(QLatin1Char('\n')))
        lines << (l.isEmpty() ? QStringLiteral(">") : QStringLiteral("> ") + l);
    return lines.join(QLatin1Char('\n'));
}

QString Compose::completeTo(const QString &field, const QString &name, const QString &addr)
{
    const int cut = qMax(field.lastIndexOf(QLatin1Char(',')), field.lastIndexOf(QLatin1Char(';')));
    const QString prefix = cut < 0 ? QString() : field.left(cut + 1) + QLatin1Char(' ');
    QString shown = name;
    if (shown.contains(QLatin1Char(',')))
        shown = QLatin1Char('"') + shown + QLatin1Char('"');
    return prefix + (shown.isEmpty() ? addr : shown + QStringLiteral(" <") + addr + QLatin1Char('>'))
        + QStringLiteral(", ");
}

} // namespace om
