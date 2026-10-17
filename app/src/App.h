#pragma once
// Window controller: mailboxes, folders, the mail list, search and maild events;
// actions on the mail, their undo, auto-read and the window's keys.
#include "Nav.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <functional>

namespace om {

class Client;
class Compose;
class Letter;
class MailList;
class Theme;

class App : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString page READ page NOTIFY changed)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY changed)
    Q_PROPERTY(QVariantList accountChoices READ accountChoices NOTIFY changed)
    Q_PROPERTY(int accountIndex READ accountIndex NOTIFY changed)
    Q_PROPERTY(QVariantList folderChoices READ folderChoices NOTIFY changed)
    Q_PROPERTY(int folderIndex READ folderIndex NOTIFY changed)
    Q_PROPERTY(bool unreadOnly READ unreadOnly NOTIFY changed)
    Q_PROPERTY(QString query READ query NOTIFY changed)
    Q_PROPERTY(bool searching READ searching NOTIFY changed)
    Q_PROPERTY(QString foundText READ foundText NOTIFY changed)
    Q_PROPERTY(bool remotePending READ remotePending NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(bool statusError READ statusError NOTIFY changed)
    Q_PROPERTY(QString note READ note NOTIFY changed)
    Q_PROPERTY(bool noteError READ noteError NOTIFY changed)
    Q_PROPERTY(bool trashView READ trashView NOTIFY changed)
    Q_PROPERTY(bool letterAtTop READ letterAtTop WRITE setLetterAtTop)   // QML keeps it = letterView.atTop
public:
    App(Client *client, MailList *list, Letter *letter, Compose *compose, Nav *nav, Theme *theme,
        QObject *parent = nullptr);
    void start(qint64 openId = 0);   // openId: the letter to show first (`--open`)

    QString page() const { return m_page; }            // "main" / "settings"
    QVariantList accounts() const;                     // {email, name, provider, statusText, authFailed}
    QVariantList accountChoices() const;               // "All mailboxes" + addresses; empty with one mailbox
    int accountIndex() const;                          // in `accounts`, -1: all mailboxes (accountChoices[accountIndex + 1])
    QVariantList folderChoices() const;                // {label, unread, separatorBefore}
    int folderIndex() const;
    bool unreadOnly() const { return m_unreadOnly; }
    QString query() const { return m_query; }
    bool searching() const { return m_searching; }
    QString foundText() const { return m_foundText; }
    bool remotePending() const { return m_remotePending; }
    QString statusText() const { return m_statusText; }
    bool statusError() const { return m_statusError; }
    QString note() const { return m_note; }
    bool noteError() const { return m_noteError; }
    bool trashView() const;                            // a Trash or Junk folder
    bool letterAtTop() const { return m_letterAtTop; }
    void setLetterAtTop(bool v) { m_letterAtTop = v; }

    Q_INVOKABLE void pickAccount(int index);           // as accountIndex: -1 all mailboxes
    Q_INVOKABLE void pickFolder(int index);
    Q_INVOKABLE void setQuery(const QString &q);
    Q_INVOKABLE void toggleUnread();
    Q_INVOKABLE void openSettings();
    Q_INVOKABLE void closeSettings();
    Q_INVOKABLE void suggest(const QString &prefix);   // "To" completion -> compose.suggestions
    void openMessage(qint64 id);

    // focus: "" the window, "search" the search field, "compose" the form; true: the key is eaten
    Q_INVOKABLE bool key(int key, quint32 nativeScanCode, int modifiers, bool autoRepeat, const QString &focus);
    Q_INVOKABLE void remove();
    Q_INVOKABLE void toggleRead();
    Q_INVOKABLE void toggleFlag();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void reply();
    Q_INVOKABLE void composeNew();
    Q_INVOKABLE void copyText();
    Q_INVOKABLE void openOriginal();
    Q_INVOKABLE void pressAction(int col);             // 0 Compose, 1 Reply, 2 Delete, 3 Unread, 4 Settings
    Q_INVOKABLE void retryOutbox();                    // the open letter, if unsent
    Q_INVOKABLE void discardOutbox();
    Q_INVOKABLE void send();
    Q_INVOKABLE void clickRow(int index, int modifiers);

Q_SIGNALS:
    void changed();
    void raiseRequested();
    void closeRequested();
    void focusSearch();
    void leaveSearch();                    // the search field gives the keys back to the window
    void openPicker(const QString &which); // "account" / "folder"
    void scroll(int cmd, int dir);         // int(Cmd::ScrollLines / ScrollPages / ScrollTop / ScrollEnd) -> LetterView
    void pickFiles();                      // Ctrl+O in the form -> FileDialog

private:
    void connectedChanged();
    void onEvent(const QString &name, const QJsonObject &payload);
    void requestStatus();
    void setStatus();
    void setNote(const QString &text, bool error);
    void loadFolders(std::function<void()> then = {});
    void remember(const QString &account, const QJsonArray &folders);
    void setPlace();
    QJsonObject listRequest(int offset, int limit) const;
    void loadList(bool fresh, qint64 target = 0);
    void keepCursorRow(QJsonArray &rows) const;
    void loadMore();
    void cursorMoved(bool force = false);
    void selectView(const QString &account, qint64 target);
    void runSearch();
    void endSearch();
    void remoteHits(const QJsonObject &payload);
    void updateFound();
    template <typename F>
    std::function<void(const QJsonValue &, const QString &)> ok(F onResult);

    QString composeAccount() const;   // the viewed mailbox, else the first
    QList<qint64> targets();
    bool anyWithout(const QList<qint64> &ids, const QString &field) const;
    void act(const QList<qint64> &ids, const QString &op, bool undoable);
    void discard(qint64 outbox);
    void autoRead();
    bool windowKey(const KeyPress &k, bool repeat);
    bool searchKey(const KeyPress &k, bool repeat);
    bool composeKey(const KeyPress &k, bool repeat);
    void back();
    NavShape shape() const;
    void run(const Step &st, bool repeat);

    Client *m_client;
    MailList *m_list;
    Letter *m_letter;
    Compose *m_compose;
    Nav *m_nav;
    Theme *m_theme;

    QString m_page = QStringLiteral("main");
    QJsonArray m_accounts, m_status, m_folders;
    QString m_account;          // the mailbox viewed, empty: all
    QJsonObject m_folder;       // the folder viewed (FolderInfo), empty: Inbox
    QHash<QString, QHash<QString, QString>> m_places;   // mailbox -> folder path -> label
    bool m_unreadOnly = false;
    qint64 m_openId = 0;
    bool m_synced = false;      // `sync {}` once per window
    bool m_opened = false;      // the window's first list has arrived
    quint64 m_gen = 0;          // view changes and list/search loads: replies of an older one are dropped
    bool m_paging = false, m_end = false;
    bool m_placing = false;     // rows being replaced: the cursor settles before the letter follows
    QString m_cursorKey;        // the row the letter last followed

    QString m_query, m_foundText;
    bool m_searching = false, m_remotePending = false;
    qint64 m_token = -1;        // -1 while our `search` is on its way
    QList<QJsonObject> m_early; // search_remote events that came before the `search` reply
    int m_remoteLeft = 0;       // mailboxes yet to send their search_remote

    QString m_statusText, m_note;
    bool m_statusError = false, m_noteError = false;
    QTimer m_searchTimer, m_remoteTimer, m_noteTimer;

    QList<qint64> m_undo;       // batches of the user's actions, the latest last
    Confirm m_confirm;          // second presses: delete forever, discard unsent mail
    QTimer m_readTimer;
    bool m_sending = false;
    bool m_byUser = false;      // the cursor moves by a key, a click or a new view's placement: auto-read may start
    bool m_letterAtTop = true;
};

} // namespace om
