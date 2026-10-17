#include "App.h"
#include "Client.h"
#include "Compose.h"
#include "Format.h"
#include "Highlight.h"
#include "I18n.h"
#include "Letter.h"
#include "MailList.h"
#include "Theme.h"
#include <QPointer>
#include <algorithm>

namespace om {

namespace {
constexpr int kPage = 50;

QString email(const QJsonValue &account) { return account.toObject().value(QStringLiteral("email")).toString(); }
} // namespace

// Reply handler: `onResult` on success while the App lives; an error becomes the note
// (unless the service is gone: the status line says so).
template <typename F>
std::function<void(const QJsonValue &, const QString &)> App::ok(F onResult)
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
    connect(list, &MailList::cursorChanged, this, [this] { cursorMoved(); });
    connect(letter, &Letter::note, this, &App::setNote);
    setPlace();
}

void App::start(qint64 openId)
{
    m_openId = openId;
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
    m_client->request({{"cmd", "accounts"}}, ok([this](const QJsonValue &r) {
        m_accounts = r.toArray();
        if (m_accounts.isEmpty())
            m_page = QStringLiteral("settings");
        if (m_accounts.size() == 1)
            m_account = email(m_accounts.at(0));
        else if (accountIndex() < 0)
            m_account.clear();
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
// The window's first list is always fresh, aimed at `--open`.
void App::loadList(bool fresh, qint64 target)
{
    if (!m_opened) {
        fresh = true;
        if (target == 0)
            target = m_openId;
    }
    const quint64 gen = ++m_gen;
    m_paging = false;
    m_end = false;
    const int limit = fresh ? kPage : std::max(kPage, m_list->rowCount());
    m_client->request(listRequest(0, limit), ok([this, gen, fresh, limit, target](const QJsonValue &r) {
        if (gen != m_gen)
            return;
        m_opened = true;
        const QJsonArray rows = r.toArray();
        m_end = rows.size() < limit;
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
    m_page = QStringLiteral("main");
    selectView(m_accounts.size() == 1 ? email(m_accounts.at(0)) : QString(), id);
}

void App::openSettings()
{
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

} // namespace om
