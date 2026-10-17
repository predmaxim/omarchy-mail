#include "App.h"
#include "Client.h"
#include "Compose.h"
#include "Format.h"
#include "Highlight.h"
#include "I18n.h"
#include "Letter.h"
#include "MailList.h"
#include "Theme.h"
#include <QDateTime>
#include <QPointer>
#include <QScopedValueRollback>
#include <algorithm>

namespace om {

namespace {
constexpr int kPage = 50;

QString email(const QJsonValue &account) { return account.toObject().value(QStringLiteral("email")).toString(); }
} // namespace

// Reply handler: `onResult` on success while the App lives; an error becomes the note
// (unless the service is gone: the status line says so).
std::function<void(const QJsonValue &, const QString &)> App::ok(std::function<void(const QJsonValue &)> onResult)
{
    return [self = QPointer<App>(this), onResult = std::move(onResult)](const QJsonValue &result, const QString &err) {
        if (!self)
            return;
        if (err.isEmpty())
            onResult(result);
        else if (self->m_client->connected())
            self->setNote(err, true);
    };
}

App::App(Client *client, MailList *list, Letter *letter, Compose *compose, Nav *nav, Theme *theme, QObject *parent)
    : QObject(parent), m_client(client), m_list(list), m_letter(letter), m_compose(compose), m_nav(nav), m_theme(theme)
{
    m_searchTimer.setSingleShot(true);
    m_searchTimer.setInterval(150);
    connect(&m_searchTimer, &QTimer::timeout, this, &App::runSearch);
    m_remoteTimer.setSingleShot(true);
    m_remoteTimer.setInterval(30000);
    connect(&m_remoteTimer, &QTimer::timeout, this, [this] {
        m_remotePending = false;
        Q_EMIT changed();
    });
    m_noteTimer.setSingleShot(true);
    m_noteTimer.setInterval(6000);
    connect(&m_noteTimer, &QTimer::timeout, this, [this] {
        m_note.clear();
        Q_EMIT changed();
    });
    m_readTimer.setSingleShot(true);
    m_readTimer.setInterval(1000);   // auto-read: arrowing through the list doesn't read it all
    connect(&m_readTimer, &QTimer::timeout, this, &App::autoRead);
    connect(list, &MailList::cursorChanged, this, [this] { cursorMoved(); });
    connect(letter, &Letter::note, this, &App::setNote);
    connect(letter, &Letter::quotesOpened, this, [this] {   // W39: Enter again must not open a link that slid in
        if (m_nav->row() == int(Row::Links))
            m_nav->reset(Row::Text);
    });
    connect(letter, &Letter::composeTo, this, [this](const QUrl &url) { m_compose->startMailto(url, composeAccount()); });
    setPlace();
}

void App::start(qint64 openId)
{
    m_freshTarget = openId;
    connect(m_client, &Client::connectedChanged, this, &App::connectedChanged);
    connect(m_client, &Client::event, this, &App::onEvent);
    if (m_client->connected()) {
        connectedChanged();
    } else {
        setStatus();
        Q_EMIT changed();
    }
}

// ---- properties ----

QVariantList App::accounts() const
{
    QVariantList out;
    for (const QJsonValue &v : m_accounts) {
        const QJsonObject a = v.toObject();
        QJsonObject st;
        for (const QJsonValue &s : m_status)
            if (email(s) == email(a))
                st = s.toObject();
        out << QVariantMap{{"email", email(a)},
                           {"name", a.value(QStringLiteral("name")).toString()},
                           {"provider", a.value(QStringLiteral("provider")).toString()},
                           {"statusText", st.isEmpty() ? QString() : format::statusLine(true, QJsonArray{st}).text},
                           {"authFailed", st.value(QStringLiteral("status")).toString() == QLatin1String("auth_failed")}};
    }
    return out;
}

QVariantList App::accountChoices() const
{
    if (m_accounts.size() < 2)
        return {};
    QVariantList out{t("All mailboxes")};
    for (const QJsonValue &v : m_accounts)
        out << email(v);
    return out;
}

int App::accountIndex() const
{
    for (int i = 0; i < m_accounts.size(); ++i)
        if (email(m_accounts.at(i)) == m_account)
            return i;
    return -1;
}

QVariantList App::folderChoices() const
{
    QVariantList out;
    for (const QJsonValue &v : m_folders) {
        const QJsonObject f = v.toObject();
        const QString role = f.value(QStringLiteral("role")).toString();
        out << QVariantMap{{"label", format::folderLabel(role, f.value(QStringLiteral("name")).toString())},
                           {"unread", f.value(QStringLiteral("unread")).toInt()},
                           {"separatorBefore", role == QLatin1String("junk")}};
    }
    return out;
}

int App::folderIndex() const
{
    for (int i = 0; i < m_folders.size(); ++i) {
        const QJsonObject f = m_folders.at(i).toObject();
        if (f.value(QStringLiteral("id")) == m_folder.value(QStringLiteral("id"))
            && f.value(QStringLiteral("role")) == m_folder.value(QStringLiteral("role")))
            return i;
    }
    return -1;
}

bool App::trashView() const
{
    const QString role = m_folder.value(QStringLiteral("role")).toString();
    return role == QLatin1String("trash") || role == QLatin1String("junk");
}

// ---- service ----

void App::connectedChanged()
{
    setStatus();   // lost: "Mail service is not running"
    Q_EMIT changed();
    if (!m_client->connected())
        return;
    if (!m_letter->error().isEmpty())   // it failed while maild was away
        m_letter->reload();
    m_client->request({{"cmd", "accounts"}}, ok([this](const QJsonValue &r) {
        m_accounts = r.toArray();
        if (m_accounts.isEmpty())
            m_page = QStringLiteral("settings");
        if (m_accounts.size() == 1)
            m_account = email(m_accounts.at(0));
        else if (accountIndex() < 0)
            m_account.clear();
        if (!m_pendingMailto.isEmpty())
            openMailto(std::exchange(m_pendingMailto, {}));
        requestStatus();
        loadFolders([this] {
            if (m_searching)
                runSearch();
            else
                loadList(false);
            if (!std::exchange(m_synced, true))
                m_client->request({{"cmd", "sync"}});   // the other folders refresh when the window opens
            if (m_accounts.size() > 1)   // folder names for the places of search rows
                for (const QJsonValue &v : std::as_const(m_accounts)) {
                    const QString a = email(v);
                    m_client->request({{"cmd", "folders"}, {"account", a}},
                                      ok([this, a](const QJsonValue &f) { remember(a, f.toArray()); }));
                }
        });
        Q_EMIT changed();
    }));
}

void App::onEvent(const QString &name, const QJsonObject &p)
{
    if (name == QLatin1String("changed")) {
        if (p.value(QStringLiteral("folders")).toBool())
            loadFolders();
        // ponytail: search rows may be stale vs phone changes until the next search
        if (!m_searching)
            loadList(false);
        const qint64 open = m_letter->id();
        const QJsonArray ids = p.value(QStringLiteral("ids")).toArray();
        if (open != 0 && std::any_of(ids.begin(), ids.end(), [open](const QJsonValue &v) { return v.toInteger() == open; }))
            m_letter->reload();
        requestStatus();
    } else if (name == QLatin1String("status")) {
        requestStatus();   // the event has no `queued`
    } else if (name == QLatin1String("open")) {
        openMessage(p.value(QStringLiteral("id")).toInteger());
        Q_EMIT raiseRequested();
    } else if (name == QLatin1String("search_remote")) {
        remoteHits(p);
    } else if (name == QLatin1String("action_failed")) {
        setNote(p.value(QStringLiteral("error")).toString(), true);
    } else if (name == QLatin1String("send_failed")) {
        setNote(t("Not sent: %1").arg(p.value(QStringLiteral("error")).toString()), true);
    }
}

void App::requestStatus()
{
    m_client->request({{"cmd", "status"}}, ok([this](const QJsonValue &r) {
        m_status = r.toObject().value(QStringLiteral("accounts")).toArray();
        setStatus();
        Q_EMIT changed();
    }));
}

void App::setStatus()
{
    const format::StatusLine s = format::statusLine(m_client->connected(), m_status);
    m_statusText = s.text;
    m_statusError = s.error;
}

void App::setNote(const QString &text, bool error)
{
    m_note = text;
    m_noteError = error;
    m_noteTimer.start();
    Q_EMIT changed();
}

// ---- folders and the list ----

// `then` runs even when the view changed meanwhile: it reads the current view.
void App::loadFolders(std::function<void()> then)
{
    QJsonObject req{{"cmd", "folders"}};
    if (!m_account.isEmpty())
        req.insert(QStringLiteral("account"), m_account);
    const QString account = m_account;
    m_client->request(req, ok([this, account, then = std::move(then)](const QJsonValue &r) {
        if (account == m_account) {
            m_folders = r.toArray();
            if (!account.isEmpty())
                remember(account, m_folders);
            if (folderIndex() < 0) {   // gone, or none chosen yet: Inbox
                const bool gone = !m_folder.isEmpty();
                m_folder = {};
                for (const QJsonValue &v : std::as_const(m_folders))
                    if (v.toObject().value(QStringLiteral("role")).toString() == QLatin1String("inbox")) {
                        m_folder = v.toObject();
                        break;
                    }
                if (gone)
                    loadList(true);
            }
            Q_EMIT changed();
        }
        if (then)
            then();
    }));
}

void App::remember(const QString &account, const QJsonArray &folders)
{
    QHash<QString, QString> &labels = m_places[account];
    labels.clear();
    for (const QJsonValue &v : folders) {
        const QJsonObject f = v.toObject();   // `path` is only a key here: never shown, never decoded
        labels.insert(f.value(QStringLiteral("path")).toString(),
                      format::folderLabel(f.value(QStringLiteral("role")).toString(), f.value(QStringLiteral("name")).toString()));
    }
    if (m_searching)
        setPlace();
}

// Under each row: the mailbox in the unified view; mailbox · folder in search; nothing in one mailbox's view.
void App::setPlace()
{
    m_list->setPlace([this](const QJsonObject &s) {
        const QString account = s.value(QStringLiteral("account")).toString();
        if (m_searching) {
            const QString folder = m_places.value(account).value(s.value(QStringLiteral("folder_path")).toString());
            return folder.isEmpty() ? account : account + QStringLiteral(" · ") + folder;
        }
        return m_account.isEmpty() ? account : QString();
    });
}

QJsonObject App::listRequest(int offset, int limit) const
{
    QJsonObject req{{"cmd", "list"}, {"unread_only", m_unreadOnly}, {"offset", offset}, {"limit", limit}};
    const QJsonValue id = m_folder.value(QStringLiteral("id"));
    if (id.isDouble()) {
        req.insert(QStringLiteral("folder"), id);
    } else {
        req.insert(QStringLiteral("role"), m_folder.value(QStringLiteral("role")).toString(QStringLiteral("inbox")));
        if (!m_account.isEmpty())
            req.insert(QStringLiteral("account"), m_account);
    }
    return req;
}

// fresh: a new view, the cursor goes to `target` or the first unread; otherwise a reload keeping the cursor.
// A `target` not among the rows still opens on the right, the cursor stays.
// A reload while a fresh load is on its way replaces it, fresh and aimed as it was;
// the window's first list is one, aimed at `--open`.
void App::loadList(bool fresh, qint64 target)
{
    if (fresh) {
        m_freshPending = true;
        m_freshTarget = target;
    } else if (m_freshPending) {
        fresh = true;
        target = m_freshTarget;
    }
    const quint64 gen = ++m_gen;
    m_paging = false;
    m_end = false;
    const int limit = fresh ? kPage : std::max(kPage, m_list->rowCount());
    m_client->request(listRequest(0, limit), ok([this, gen, fresh, limit, target](const QJsonValue &r) {
        if (gen != m_gen)
            return;
        m_freshPending = false;
        QJsonArray rows = r.toArray();
        m_end = rows.size() < limit;
        if (!fresh && m_unreadOnly)
            keepCursorRow(rows);
        const QScopedValueRollback<bool> placed(m_byUser, fresh);   // a new view's letter is one the user went to
        int at = -1;
        for (int i = 0; target != 0 && i < rows.size() && at < 0; ++i)
            if (rows.at(i).toObject().value(QStringLiteral("id")).toInteger() == target)
                at = i;
        m_placing = true;
        m_list->setRows(rows);
        if (fresh)
            m_list->setCursor(at >= 0 ? at : m_list->initialCursor());
        m_placing = false;
        if (target != 0 && at < 0) {
            m_cursorKey = m_list->data(m_list->index(m_list->cursor()), MailList::KeyRole).toString();
            m_letter->show({{"id", target}});
        } else {
            cursorMoved(target != 0 && m_letter->id() != target);
        }
    }));
}

// The unread-only view: a letter just read stays where it was while the cursor is on it, instead of
// the next unread sliding under the reader. It goes with the first reload after the cursor leaves.
// ponytail: while it stays, the next page's offset counts it, so one row may wait for the next reload
void App::keepCursorRow(QJsonArray &rows) const
{
    const QJsonObject cur = m_list->cursorRow();
    const qint64 id = cur.value(QStringLiteral("id")).toInteger();
    if (id == 0)
        return;
    QHash<qint64, int> at;
    for (int i = 0; i < rows.size(); ++i)
        at.insert(rows.at(i).toObject().value(QStringLiteral("id")).toInteger(), i);
    if (at.contains(id))
        return;
    int pos = int(rows.size());   // before the first row below it that is still there, else last
    for (int i = m_list->cursor() + 1; i < m_list->rowCount() && pos == rows.size(); ++i)
        pos = at.value(m_list->row(i).value(QStringLiteral("id")).toInteger(), pos);
    rows.insert(pos, cur);   // with its flags as the list has them: read
}

void App::loadMore()
{
    m_paging = true;
    const quint64 gen = m_gen;
    m_client->request(listRequest(m_list->rowCount(), kPage), ok([this, gen](const QJsonValue &r) {
        if (gen != m_gen)
            return;
        m_paging = false;
        const QJsonArray rows = r.toArray();
        m_end = rows.size() < kPage;
        m_list->appendRows(rows);
    }));
}

// The letter follows the cursor; near the end of the loaded rows the next page comes.
void App::cursorMoved(bool force)
{
    if (m_placing)
        return;
    const int c = m_list->cursor();
    const QString key = m_list->data(m_list->index(c), MailList::KeyRole).toString();
    if (force || key != m_cursorKey) {
        m_cursorKey = key;
        if (m_byUser)   // auto-read: never a letter a reload or a search slid under the cursor
            m_readTimer.start();
        else
            m_readTimer.stop();
        if (c < 0)
            m_letter->clear();
        else
            m_letter->show(m_list->cursorRow());
    }
    if (!m_searching && !m_paging && !m_end && c >= 0 && c >= m_list->rowCount() - 10)
        loadMore();
}

void App::selectView(const QString &account, qint64 target)
{
    ++m_gen;   // the old view's replies in flight
    endSearch();
    m_account = account;
    m_folder = {};
    m_folders = {};
    loadFolders([this, target] { loadList(true, target); });
    Q_EMIT changed();
}

void App::pickAccount(int index)
{
    if (m_accounts.size() < 2)
        return;
    selectView(index >= 0 && index < m_accounts.size() ? email(m_accounts.at(index)) : QString(), 0);
}

void App::pickFolder(int index)
{
    if (index < 0 || index >= m_folders.size())
        return;
    endSearch();
    m_folder = m_folders.at(index).toObject();
    loadList(true);
    const QJsonValue id = m_folder.value(QStringLiteral("id"));
    if (id.isDouble())
        m_client->request({{"cmd", "sync"}, {"folder", id}});
    Q_EMIT changed();
}

void App::toggleUnread()
{
    endSearch();
    m_unreadOnly = !m_unreadOnly;
    loadList(true);
    Q_EMIT changed();
}

void App::openMessage(qint64 id)
{
    m_compose->close();   // its draft kept: the letter must be in sight
    m_page = QStringLiteral("main");
    selectView(m_accounts.size() == 1 ? email(m_accounts.at(0)) : QString(), id);
}

void App::openMailto(const QUrl &url)
{
    if (m_accounts.isEmpty()) {
        m_pendingMailto = url;   // composeAccount() needs the first mailbox
        return;
    }
    m_page = QStringLiteral("main");   // not behind the settings page
    m_compose->startMailto(url, composeAccount());
    Q_EMIT changed();
}

void App::openSettings()
{
    m_compose->close();   // its draft kept: the form must not show through the page
    if (m_compose->isOpen())
        return;   // the draft could not be saved: the form stays, with the error
    m_page = QStringLiteral("settings");
    Q_EMIT changed();
}

void App::closeSettings()
{
    m_page = QStringLiteral("main");
    Q_EMIT changed();
}

void App::suggest(const QString &prefix)
{
    if (prefix.trimmed().isEmpty()) {
        m_compose->setSuggestions({});
        return;
    }
    m_client->request({{"cmd", "contacts"}, {"prefix", prefix}},
                      ok([this](const QJsonValue &r) { m_compose->setSuggestions(r.toArray().toVariantList()); }));
}

// ---- search ----

void App::setQuery(const QString &q)
{
    if (q == m_query)
        return;
    if (q.trimmed().isEmpty()) {
        const bool was = m_searching;
        endSearch();
        if (was)
            loadList(true);
    } else {
        m_searching = true;
        m_searchTimer.start();
    }
    m_query = q;
    Q_EMIT changed();
}

void App::runSearch()
{
    const quint64 gen = ++m_gen;
    const QString q = m_query;
    m_token = -1;
    m_early.clear();
    m_client->request({{"cmd", "search"}, {"q", q}}, ok([this, gen, q](const QJsonValue &r) {
        if (gen != m_gen)
            return;
        const QJsonObject o = r.toObject();
        m_token = o.value(QStringLiteral("token")).toInteger(-1);
        m_remoteLeft = int(m_accounts.size());
        m_remotePending = m_remoteLeft > 0;
        if (m_remotePending)
            m_remoteTimer.start();
        const QStringList words = highlight::words(q);
        m_list->setWords(words, m_theme->mark());
        m_letter->setWords(words);
        m_placing = true;
        m_list->setRows(o.value(QStringLiteral("local")).toArray());
        m_list->setCursor(0);
        m_placing = false;
        cursorMoved();
        updateFound();
        for (const QJsonObject &e : std::exchange(m_early, {}))
            remoteHits(e);
    }));
}

void App::remoteHits(const QJsonObject &p)
{
    if (!m_searching)
        return;
    if (m_token < 0) {   // maild may report before our `search` reply: kept until its token is known
        m_early << p;
        return;
    }
    if (p.value(QStringLiteral("token")).toInteger(-2) != m_token)
        return;   // an older search's
    m_list->appendRemote(p.value(QStringLiteral("hits")).toArray());
    if (--m_remoteLeft <= 0) {
        m_remotePending = false;
        m_remoteTimer.stop();
    }
    updateFound();
}

void App::updateFound()
{
    m_foundText = tn("Found %n", m_list->rowCount()) + QStringLiteral(" · ") + t("in all mailboxes and folders");
    Q_EMIT changed();
}

// Leaves search (the caller loads the folder's list).
void App::endSearch()
{
    m_searchTimer.stop();
    m_remoteTimer.stop();
    if (!m_searching)
        return;
    m_query.clear();
    m_searching = false;
    m_remotePending = false;
    m_token = -1;
    m_early.clear();
    m_foundText.clear();
    m_list->setWords({}, m_theme->mark());
    m_letter->setWords({});
}

// ---- actions ----

QString App::composeAccount() const
{
    return m_account.isEmpty() && !m_accounts.isEmpty() ? email(m_accounts.at(0)) : m_account;
}

// The ids an action applies to: the selection, else the letter the user sees: the cursor row's, or one
// that isn't among the rows (W16: `--open`, a notification). Server search hits have none, they can't be changed.
QList<qint64> App::targets()
{
    const QJsonObject cur = m_list->cursorRow();
    const qint64 shown = m_letter->id();
    if (m_list->selectedCount() == 0 && !cur.contains(QStringLiteral("remote"))
        && shown != cur.value(QStringLiteral("id")).toInteger())
        return shown > 0 ? QList<qint64>{shown} : QList<qint64>{};
    const QList<qint64> ids = m_list->targetIds();
    if (ids.isEmpty() && (m_list->selectedCount() > 0 || m_list->cursorRow().contains(QStringLiteral("remote"))))
        setNote(t("Server search results can't be changed"), false);
    return ids;
}

// Whether one of `ids` has `field` off: as its row has it, or the shown letter outside the rows.
bool App::anyWithout(const QList<qint64> &ids, const QString &field) const
{
    QList<qint64> left = ids;
    for (int i = 0; i < m_list->rowCount(); ++i) {
        const QJsonObject r = m_list->row(i);
        if (left.removeOne(r.value(QStringLiteral("id")).toInteger()) && !r.value(field).toBool())
            return true;
    }
    return left.contains(m_letter->id())
        && !m_letter->full().value(QStringLiteral("summary")).toObject().value(field).toBool();
}

// The rows show the change at once; when maild refuses it, they are loaded again from maild.
void App::act(const QList<qint64> &ids, const QString &op, bool undoable)
{
    QJsonArray a;
    for (const qint64 id : ids)
        a << id;
    m_client->request({{"cmd", "act"}, {"ids", a}, {"op", op}},
                      [self = QPointer<App>(this), undoable](const QJsonValue &r, const QString &err) {
                          if (!self)
                              return;
                          if (err.isEmpty()) {
                              const qint64 batch = r.toObject().value(QStringLiteral("batch")).toInteger();
                              if (undoable && batch > 0)
                                  self->m_undo << batch;
                              return;
                          }
                          if (self->m_client->connected())
                              self->setNote(err, true);
                          if (!self->m_searching)   // ponytail: search rows stay as changed until the next search
                              self->loadList(false);
                      });
}

void App::remove()
{
    const QScopedValueRollback<bool> user(m_byUser, true);   // a key or the Delete button: the next letter is theirs
    const qint64 id = m_list->cursorRow().value(QStringLiteral("id")).toInteger();
    if (id < 0 && m_list->selectedCount() == 0 && m_letter->id() == id) {
        discard(id);
        return;
    }
    const QList<qint64> ids = targets();
    if (ids.isEmpty())
        return;
    if (trashView() && !m_searching) {   // search rows come from every folder: there `d` moves to Trash
        QStringList same;   // the second press confirms these rows, not whatever is under the cursor then
        for (const qint64 i : ids)
            same << QString::number(i);
        if (!m_confirm.press(QStringLiteral("delete_forever ") + same.join(QLatin1Char(',')),
                             QDateTime::currentMSecsSinceEpoch())) {
            setNote(t("Press d again to delete forever"), false);
            return;
        }
        act(ids, QStringLiteral("delete_forever"), false);   // undo can't bring these back
    } else {
        act(ids, QStringLiteral("delete"), true);
    }
    m_list->removeIds(ids);
    if (m_letter->id() != m_list->cursorRow().value(QStringLiteral("id")).toInteger())
        cursorMoved(true);   // the letter gone wasn't a row: the pane follows the cursor again
}

void App::discard(qint64 outbox)
{
    if (!m_confirm.press(QStringLiteral("discard %1").arg(outbox), QDateTime::currentMSecsSinceEpoch())) {
        setNote(t("Press d again to discard the unsent message"), false);
        return;
    }
    m_client->request({{"cmd", "outbox_discard"}, {"outbox", outbox}}, ok([](const QJsonValue &) {}));
}

void App::toggleRead()
{
    const QList<qint64> ids = targets();
    if (ids.isEmpty())
        return;
    m_readTimer.stop();   // a letter just made unread stays so
    const bool read = anyWithout(ids, QStringLiteral("seen"));
    act(ids, read ? QStringLiteral("read") : QStringLiteral("unread"), true);
    m_list->setFlags(ids, read, std::nullopt);
}

void App::toggleFlag()
{
    const QList<qint64> ids = targets();
    if (ids.isEmpty())
        return;
    const bool flag = anyWithout(ids, QStringLiteral("flagged"));
    act(ids, flag ? QStringLiteral("flag") : QStringLiteral("unflag"), true);
    m_list->setFlags(ids, std::nullopt, flag);
}

void App::reply()
{
    if (m_letter->id() > 0 && !m_letter->full().isEmpty())
        m_compose->startReply(m_letter->full());
}

void App::composeNew() { m_compose->startNew(composeAccount()); }
void App::copyText() { m_letter->copyText(); }
void App::openOriginal() { m_letter->openOriginal(); }

void App::pressAction(int col)
{
    if (m_compose->isOpen() && col != 4)
        return;   // the list is only a backdrop under the form; the gear closes the form
    switch (col) {
    case 0: composeNew(); break;
    case 1: reply(); break;
    case 2: remove(); break;
    case 3: toggleUnread(); break;
    case 4: m_page == QLatin1String("main") ? openSettings() : closeSettings(); break;   // the gear, the back arrow
    }
}

void App::retryOutbox()
{
    if (m_letter->id() < 0)
        m_client->request({{"cmd", "outbox_retry"}, {"outbox", m_letter->id()}}, ok([](const QJsonValue &) {}));
}

void App::discardOutbox()
{
    if (m_letter->id() < 0)
        discard(m_letter->id());
}

void App::send()
{
    if (m_sending || !m_compose->isOpen())
        return;   // one mail per form, however fast the second Ctrl+Enter comes
    QString err;
    const QJsonObject req = m_compose->request(&err);
    if (!err.isEmpty()) {
        m_compose->setNote(err, true);
        return;
    }
    m_sending = true;
    // maild may take a while (it reads the attachments): the form may be closed or on another draft by then
    const Compose::Draft draft = m_compose->draft();
    m_client->requestSlow(req, [self = QPointer<App>(this), draft](const QJsonValue &, const QString &e) {
        if (!self)
            return;
        self->m_sending = false;
        if (e.isEmpty()) {
            self->m_compose->sent(draft);
            self->setNote(t("Sending…"), false);
        } else if (self->m_compose->shows(draft)) {
            self->m_compose->setNote(e, true);
        } else {
            self->setNote(t("Not sent: %1").arg(e), true);
        }
    });
}

void App::clickRow(int index, int modifiers)
{
    const QScopedValueRollback<bool> user(m_byUser, true);
    const auto mods = Qt::KeyboardModifiers::fromInt(modifiers);
    m_list->click(index, mods.testFlag(Qt::ControlModifier), mods.testFlag(Qt::ShiftModifier));
    m_nav->reset(Row::List);
}

// ---- undo and auto-read ----

void App::undo()
{
    if (m_undo.isEmpty()) {
        setNote(t("Nothing to undo"), false);
        return;
    }
    // the undo's own batch isn't stacked: the next Ctrl+Z goes one more action back
    m_client->request({{"cmd", "undo"}, {"batch", m_undo.takeLast()}}, ok([](const QJsonValue &) {}));
}

// The letter under the cursor has been open for a second (the timer restarts in cursorMoved).
void App::autoRead()
{
    const QJsonObject row = m_list->cursorRow();
    const qint64 id = row.value(QStringLiteral("id")).toInteger();
    if (m_compose->isOpen()   // the form hides the letter
        || id <= 0 || id != m_letter->id() || row.value(QStringLiteral("seen")).toBool()
        || row.value(QStringLiteral("role")).toString() == QLatin1String("remote"))   // maild refuses those
        return;
    act({id}, QStringLiteral("read"), false);   // not undoable: Ctrl+Z goes to what the user did
    m_list->setFlags({id}, true, std::nullopt);
}

} // namespace om
