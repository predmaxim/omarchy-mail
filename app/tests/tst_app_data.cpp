#include "App.h"
#include "Client.h"
#include "Compose.h"
#include "FakeMaild.h"
#include "Images.h"
#include "LaunchStub.h"
#include "Letter.h"
#include "MailList.h"
#include "Nav.h"
#include "Renderer.h"
#include "Theme.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace om;
using namespace om::test;

static QJsonDocument fixture(const char *name)
{
    QFile f(QStringLiteral(OM_SOURCE_DIR "/tests/fixtures/") + QLatin1String(name));
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()) : QJsonDocument();
}

// A request as the window sent it: without the request id and FakeMaild's connection number.
static QJsonObject body(QJsonObject r)
{
    r.remove("id");
    r.remove("conn");
    return r;
}

static QJsonObject inboxList(int offset, int limit)
{
    return {{"cmd", "list"}, {"role", "inbox"}, {"unread_only", false}, {"offset", offset}, {"limit", limit}};
}

// A server search hit in Gmail's All Mail.
static QJsonObject hit(const QString &subject, int uid)
{
    return {{"id", 0}, {"account", "me@gmail.com"}, {"role", "remote"}, {"folder_path", "[Gmail]/All Mail"},
            {"date", 1700000000}, {"from", QJsonObject{{"name", "Ozon"}, {"addr", "news@ozon.example"}}},
            {"subject", subject}, {"preview", ""}, {"seen", true}, {"flagged", false}, {"has_attachments", false},
            {"queued", false}, {"failed", false},
            {"remote", QJsonObject{{"account", "me@gmail.com"}, {"folder_path", "[Gmail]/All Mail"}, {"uid", uid}}}};
}

class TstAppData : public QObject {
    Q_OBJECT
    FakeMaild *d = nullptr;
    Client *c = nullptr;
    QTemporaryDir *dir = nullptr;
    Images *images = nullptr;
    Renderer *renderer = nullptr;
    Theme *theme = nullptr;
    Letter *letter = nullptr;
    MailList *list = nullptr;
    Compose *compose = nullptr;
    Nav *nav = nullptr;
    App *app = nullptr;
    QJsonArray accounts, rows;
    QJsonObject folders;
    bool offline = false;

    QList<QJsonObject> sent(const QString &cmd) const
    {
        QList<QJsonObject> out;
        for (const QJsonObject &r : d->requests())
            if (r.value("cmd").toString() == cmd)
                out << r;
        return out;
    }
    int unifiedFolders() const
    {
        int n = 0;
        for (const QJsonObject &r : sent("folders"))
            n += !r.contains("account");
        return n;
    }
    QJsonArray slice(const QJsonObject &listReq) const
    {
        QJsonArray out;
        const int limit = listReq.value("limit").toInt();
        for (int i = listReq.value("offset").toInt(); i < rows.size() && out.size() < limit; ++i)
            out << rows.at(i);
        return out;
    }
    QString place(int row) const { return list->data(list->index(row), MailList::PlaceRole).toString(); }
    QStringList folderLabels() const
    {
        QStringList out;
        for (const QVariant &v : app->folderChoices())
            out << v.toMap().value("label").toString();
        return out;
    }
    void boot(qint64 openId = 0)
    {
        app->start(openId);
        c->start();
    }
    // Requests on the window's connection are answered in order: once this one has arrived, so has every earlier one.
    void barrier()
    {
        const auto n = sent("contacts").size();
        app->suggest("zz");
        QTRY_COMPARE(sent("contacts").size(), n + 1);
    }

private Q_SLOTS:
    void init()
    {
        accounts = fixture("accounts.json").array();
        rows = fixture("list.json").array();
        folders = fixture("folders.json").object();
        QCOMPARE(rows.size(), 60);
        offline = false;
        d = new FakeMaild;
        QVERIFY(d->start());
        d->on("accounts", [this](const QJsonObject &) { return QJsonObject{{"result", accounts}}; });
        d->on("status", [this](const QJsonObject &) {
            QJsonArray st;
            for (const QJsonValue &a : std::as_const(accounts))
                st << QJsonObject{{"email", a.toObject().value("email")},
                                  {"status", offline ? QJsonValue(QJsonObject{{"offline", "no route"}}) : QJsonValue("online")},
                                  {"queued", offline ? 3 : 0}};
            return QJsonObject{{"result", QJsonObject{{"accounts", st}}}};
        });
        d->on("folders", [this](const QJsonObject &r) {
            const QString a = r.value("account").toString();
            return QJsonObject{{"result", folders.value(a.isEmpty() ? QStringLiteral("unified") : a)}};
        });
        d->on("list", [this](const QJsonObject &r) { return QJsonObject{{"result", slice(r)}}; });
        d->on("message", [](const QJsonObject &r) {
            return QJsonObject{{"result", QJsonObject{{"summary", QJsonObject{{"id", r.value("message")}}},
                                                      {"text", "hi"}, {"html", QJsonValue::Null}}}};
        });
        c = new Client(d->path());
        c->setRetryInterval(50);
        dir = new QTemporaryDir(scratch());
        images = new Images(dir->filePath("img"));
        renderer = new Renderer;
        theme = new Theme;
        letter = new Letter(c, images, renderer, theme);
        list = new MailList;
        compose = new Compose(dir->filePath("drafts"));
        nav = new Nav;
        app = new App(c, list, letter, compose, nav, theme);
    }
    void cleanup()
    {
        delete app;
        delete nav;
        delete compose;
        delete list;
        delete letter;
        delete theme;
        delete renderer;
        delete images;
        delete dir;
        delete c;
        delete d;
    }

    void startup_sequence()
    {
        boot();
        QTRY_COMPARE(sent("folders").size(), 3);   // the unified list, then one per mailbox for search places
        const auto r = d->requests();
        QStringList order;
        for (int i = 0; i < 6; ++i)
            order << r.at(i).value("cmd").toString();
        QCOMPARE(order, QStringList({"subscribe", "accounts", "status", "folders", "list", "sync"}));
        QCOMPARE(body(r.at(3)), QJsonObject({{"cmd", "folders"}}));
        QCOMPARE(body(r.at(4)), inboxList(0, 50));
        QCOMPARE(body(r.at(5)), QJsonObject({{"cmd", "sync"}}));
        QCOMPARE(sent("folders").at(1).value("account").toString(), QString("me@gmail.com"));
        QCOMPARE(sent("folders").at(2).value("account").toString(), QString("me@yandex.ru"));

        QTRY_COMPARE(list->rowCount(), 50);
        QCOMPARE(folderLabels(), QStringList({"Inbox", "Flagged", "Sent", "Trash", "Junk"}));
        QList<bool> seps;
        for (const QVariant &v : app->folderChoices())
            seps << v.toMap().value("separatorBefore").toBool();
        QCOMPARE(seps, QList<bool>({false, false, false, false, true}));
        QCOMPARE(app->folderChoices().at(0).toMap().value("unread").toInt(), 12);
        QCOMPARE(app->folderIndex(), 0);
        QVERIFY(!app->trashView());
        QCOMPARE(app->accountChoices(), QVariantList({"All mailboxes", "me@gmail.com", "me@yandex.ru"}));
        QCOMPARE(app->accountIndex(), -1);
        QCOMPARE(app->page(), QString("main"));
        QCOMPARE(place(1), QString("me@yandex.ru"));   // the unified view names the mailbox
        QTRY_COMPARE(app->accounts().size(), 2);
        const QVariantMap a = app->accounts().at(1).toMap();
        QCOMPARE(a.value("email").toString(), QString("me@yandex.ru"));
        QCOMPARE(a.value("provider").toString(), QString("yandex"));
        QCOMPARE(a.value("authFailed").toBool(), false);
        QCOMPARE(app->statusText(), QString());
    }

    void single_account_uses_its_folders()
    {
        accounts = QJsonArray{accounts.at(0)};
        boot();
        QVERIFY(d->waitFor("sync"));
        QCOMPARE(body(sent("folders").at(0)), QJsonObject({{"cmd", "folders"}, {"account", "me@gmail.com"}}));
        QCOMPARE(body(sent("list").at(0)),
                 QJsonObject({{"cmd", "list"}, {"folder", 1}, {"unread_only", false}, {"offset", 0}, {"limit", 50}}));
        QVERIFY(app->accountChoices().isEmpty());
        QCOMPARE(app->accountIndex(), 0);
        QCOMPARE(folderLabels(), QStringList({"Inbox", "Sent", "Trash", "Junk", "Work", "All Mail"}));
        QVERIFY(app->folderChoices().at(3).toMap().value("separatorBefore").toBool());
        QTRY_COMPARE(list->rowCount(), 50);
        QCOMPARE(place(0), QString());   // one mailbox: no place under the rows

        app->pickFolder(4);   // Work
        QTRY_COMPARE(sent("sync").size(), 2);
        QCOMPARE(body(sent("list").last()),
                 QJsonObject({{"cmd", "list"}, {"folder", 7}, {"unread_only", false}, {"offset", 0}, {"limit", 50}}));
        QCOMPARE(body(sent("sync").last()), QJsonObject({{"cmd", "sync"}, {"folder", 7}}));
        QCOMPARE(app->folderIndex(), 4);
        QVERIFY(!app->trashView());
        app->pickFolder(2);
        QVERIFY(app->trashView());
    }

    void pick_account_shows_its_folders()
    {
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
        app->pickAccount(1);   // me@yandex.ru
        QCOMPARE(app->accountIndex(), 1);
        QTRY_COMPARE(folderLabels(), QStringList({"Inbox", "Sent", "Junk"}));
        QTRY_COMPARE(body(sent("list").last()),
                     QJsonObject({{"cmd", "list"}, {"folder", 3}, {"unread_only", false}, {"offset", 0}, {"limit", 50}}));
        QTRY_COMPARE(place(0), QString());
        app->pickAccount(-1);
        QTRY_COMPARE(folderLabels(), QStringList({"Inbox", "Flagged", "Sent", "Trash", "Junk"}));
        QTRY_COMPARE(body(sent("list").last()), inboxList(0, 50));
        QTRY_COMPARE(place(0), QString("me@gmail.com"));
    }

    void cursor_starts_on_first_unread()
    {
        boot();
        QTRY_COMPARE(list->cursor(), 3);
        QVERIFY(d->waitFor("message"));
        const auto m = sent("message");
        QCOMPARE(m.size(), 1);
        QCOMPARE(m.at(0).value("message").toInteger(), 57);
        QVERIFY(m.at(0).value("conn") != sent("subscribe").at(0).value("conn"));
        QCOMPARE(letter->id(), 57);
    }

    void open_id_from_args()
    {
        boot(59);
        QTRY_COMPARE(list->cursor(), 1);
        QVERIFY(d->waitFor("message"));
        QCOMPARE(sent("message").at(0).value("message").toInteger(), 59);
    }

    void open_event_raises()
    {
        boot();
        QTRY_COMPARE(list->cursor(), 3);
        app->pickFolder(3);   // Trash, then a search: the open letter brings back all mailboxes' Inbox
        app->setQuery("ozon");
        app->openSettings();
        QSignalSpy raised(app, &App::raiseRequested);
        d->emitEvent({{"event", "open"}, {"id", 7}});
        QTRY_COMPARE(raised.size(), 1);
        QVERIFY(!app->searching());
        QCOMPARE(app->query(), QString());
        QCOMPARE(app->page(), QString("main"));
        QTRY_VERIFY(!sent("message").isEmpty() && sent("message").last().value("message").toInteger() == 7);
        QCOMPARE(letter->id(), 7);
        QCOMPARE(app->folderIndex(), 0);
        QCOMPARE(body(sent("list").last()), inboxList(0, 50));
        QCOMPARE(list->cursor(), 3);   // 7 is not among the loaded rows: the letter opens, the cursor stays
        QVERIFY(sent("search").isEmpty());

        d->emitEvent({{"event", "open"}, {"id", 59}});   // a loaded row: the cursor goes there
        QTRY_COMPARE(list->cursor(), 1);
        QTRY_COMPARE(letter->id(), 59);
        QCOMPARE(raised.size(), 2);
    }

    void changed_reloads()
    {
        boot();
        QTRY_COMPARE(list->cursor(), 3);
        QTRY_COMPARE(letter->id(), 57);
        QTRY_COMPARE(letter->state(), QString());   // loaded
        const auto lists = sent("list").size(), statuses = sent("status").size();
        const int unified = unifiedFolders();
        QJsonObject fresh = rows.at(0).toObject();
        fresh["id"] = 61;
        rows.prepend(fresh);
        d->emitEvent({{"event", "changed"}, {"folders", true}, {"ids", QJsonArray{61}}});
        QTRY_COMPARE(list->cursor(), 4);
        QCOMPARE(list->cursorRow().value("id").toInteger(), 57);
        QCOMPARE(sent("list").size(), lists + 1);
        QCOMPARE(body(sent("list").last()), inboxList(0, 50));
        QCOMPARE(unifiedFolders(), unified + 1);
        QTRY_COMPARE(sent("status").size(), statuses + 1);
        QCOMPARE(letter->state(), QString());   // the same letter stays, not loaded again
        QCOMPARE(letter->id(), 57);
    }

    void changed_with_open_id_reloads_letter()
    {
        boot();
        QTRY_COMPARE(letter->id(), 57);
        QVERIFY(d->waitFor("message"));
        d->emitEvent({{"event", "changed"}, {"folders", false}, {"ids", QJsonArray{56, 57}}});
        QTRY_COMPARE(sent("message").size(), 2);
        QCOMPARE(sent("message").at(1).value("message").toInteger(), 57);
    }

    void paging_loads_more()
    {
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
        const auto lists = sent("list").size();
        list->setCursor(39);
        barrier();
        QCOMPARE(sent("list").size(), lists);
        list->setCursor(40);
        list->setCursor(45);
        list->setCursor(46);
        QTRY_COMPARE(list->rowCount(), 60);
        QCOMPARE(sent("list").size(), lists + 1);
        QCOMPARE(body(sent("list").last()), inboxList(50, 50));
        list->setCursor(55);   // the last page was short: the folder has no more
        barrier();
        QCOMPARE(sent("list").size(), lists + 1);
    }

    void search_flow()
    {
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
        QTRY_COMPARE(sent("folders").size(), 3);
        barrier();   // the mailboxes' folders have arrived: places name the folder
        QElapsedTimer clock;
        qint64 arrived = -1;
        d->on("search", [&](const QJsonObject &) {
            arrived = clock.elapsed();
            return QJsonObject{{"result", QJsonObject{{"local", QJsonArray{rows.at(0), rows.at(5)}}, {"token", 7}}}};
        });
        clock.start();
        app->setQuery("o");
        app->setQuery("oz");
        app->setQuery("ozon");
        QVERIFY(app->searching());
        QCOMPARE(app->query(), QString("ozon"));
        QTRY_COMPARE(list->rowCount(), 2);
        QVERIFY2(arrived >= 140, qPrintable(QString::number(arrived)));   // 150 ms of quiet; coarse timers may fire 5 % early
        QCOMPARE(sent("search").size(), 1);
        QCOMPARE(body(sent("search").at(0)), QJsonObject({{"cmd", "search"}, {"q", "ozon"}}));
        QVERIFY(app->remotePending());
        QCOMPARE(app->foundText(), QString("Found 2 · in all mailboxes and folders"));
        QCOMPARE(list->cursor(), 0);
        QCOMPARE(place(0), QString("me@gmail.com · Inbox"));
        QVERIFY(list->data(list->index(0), MailList::SubjectRichRole).toString().contains("<b>"));

        d->emitEvent({{"event", "search_remote"}, {"token", 6}, {"hits", QJsonArray{hit("Old search", 1)}}});
        d->emitEvent({{"event", "search_remote"}, {"token", 7}, {"hits", QJsonArray{hit("Ozon receipt", 2)}}});
        QTRY_COMPARE(list->rowCount(), 3);
        QCOMPARE(list->row(2).value("subject").toString(), QString("Ozon receipt"));
        QCOMPARE(list->data(list->index(2), MailList::GroupRole).toString(), QString("remote"));
        QCOMPARE(place(2), QString("me@gmail.com · All Mail"));
        QVERIFY(app->remotePending());   // one mailbox is still searching
        d->emitEvent({{"event", "search_remote"}, {"token", 7}, {"hits", QJsonArray()}});
        QTRY_VERIFY(!app->remotePending());
        QCOMPARE(list->rowCount(), 3);
        QCOMPARE(app->foundText(), QString("Found 3 · in all mailboxes and folders"));

        const auto lists = sent("list").size(), statuses = sent("status").size();
        d->emitEvent({{"event", "changed"}, {"folders", false}, {"ids", QJsonArray()}});
        QTRY_COMPARE(sent("status").size(), statuses + 1);
        QCOMPARE(sent("list").size(), lists);   // search rows stay until the next search

        app->setQuery("");
        QVERIFY(!app->searching());
        QVERIFY(!app->remotePending());
        QCOMPARE(app->foundText(), QString());
        QTRY_COMPARE(list->rowCount(), 50);
        QCOMPARE(body(sent("list").last()), inboxList(0, 50));
        QVERIFY(!list->data(list->index(0), MailList::SubjectRichRole).toString().contains("<b>"));
        QCOMPARE(place(0), QString("me@gmail.com"));
    }

    void status_line_from_events()
    {
        accounts = QJsonArray{accounts.at(0)};
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
        QTRY_COMPARE(app->statusText(), QString());
        const auto statuses = sent("status").size();
        offline = true;
        d->emitEvent({{"event", "status"},
                      {"accounts", QJsonArray{QJsonObject{{"email", "me@gmail.com"}, {"status", QJsonObject{{"offline", "no route"}}}}}}});
        QTRY_COMPARE(app->statusText(), QString("offline · 3 queued"));
        QVERIFY(!app->statusError());
        QCOMPARE(sent("status").size(), statuses + 1);
        QCOMPARE(app->accounts().at(0).toMap().value("statusText").toString(), QString("offline · 3 queued"));
    }

    void failure_events_set_note()
    {
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
        d->emitEvent({{"event", "action_failed"}, {"batch", 5}, {"error", "Server said no"}});
        QTRY_COMPARE(app->note(), QString("Server said no"));
        QVERIFY(app->noteError());
        d->emitEvent({{"event", "send_failed"}, {"outbox", 2}, {"error", "550 rejected"}});
        QTRY_COMPARE(app->note(), QString("Not sent: 550 rejected"));
    }

    void suggest_asks_contacts()
    {
        d->on("contacts", [](const QJsonObject &) {
            return QJsonObject{{"result", QJsonArray{QJsonObject{{"name", "Anna"}, {"addr", "anna@example.com"}}}}};
        });
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
        app->suggest("an");
        QTRY_COMPARE(compose->suggestions().size(), 1);
        QCOMPARE(body(sent("contacts").at(0)), QJsonObject({{"cmd", "contacts"}, {"prefix", "an"}}));
        QCOMPARE(compose->suggestions().at(0).toMap().value("addr").toString(), QString("anna@example.com"));
    }

    void reconnect_reloads()
    {
        boot();
        QVERIFY(d->waitFor("sync"));
        QTRY_COMPARE(list->rowCount(), 50);
        d->stop();
        QTRY_COMPARE(app->statusText(), QString("Mail service is not running"));
        QVERIFY(app->statusError());
        const auto lists = sent("list").size();
        const int unified = unifiedFolders();
        QVERIFY(d->start());
        QTRY_COMPARE(sent("subscribe").size(), 2);
        QVERIFY(sent("subscribe").at(1).value("window").toBool());
        QTRY_COMPARE(sent("list").size(), lists + 1);
        QCOMPARE(unifiedFolders(), unified + 1);
        barrier();
        QCOMPARE(sent("sync").size(), 1);   // once per window
        QTRY_COMPARE(app->statusText(), QString());
        QCOMPARE(list->cursor(), 3);
    }

    void no_accounts_opens_settings()
    {
        accounts = {};
        boot();
        QTRY_COMPARE(app->page(), QString("settings"));
        QVERIFY(app->accountChoices().isEmpty());
        app->closeSettings();
        QCOMPARE(app->page(), QString("main"));
        app->openSettings();
        QCOMPARE(app->page(), QString("settings"));
    }

    void remote_hits_before_search_reply()
    {
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
        d->on("search", [this](const QJsonObject &) {
            // maild's mailbox workers may report before the `search` reply is written
            d->emitEvent({{"event", "search_remote"}, {"token", 6}, {"hits", QJsonArray{hit("Old search", 1)}}});
            d->emitEvent({{"event", "search_remote"}, {"token", 7}, {"hits", QJsonArray{hit("Ozon receipt", 2)}}});
            d->emitEvent({{"event", "search_remote"}, {"token", 7}, {"hits", QJsonArray()}, {"error", "offline"}});
            return QJsonObject{{"result", QJsonObject{{"local", QJsonArray{rows.at(0)}}, {"token", 7}}}};
        });
        app->setQuery("ozon");
        QTRY_COMPARE(list->rowCount(), 2);
        QCOMPARE(list->row(1).value("subject").toString(), QString("Ozon receipt"));
        QVERIFY(!app->remotePending());
        QCOMPARE(app->foundText(), QString("Found 2 · in all mailboxes and folders"));
    }

    void accounts_error_is_visible()
    {
        d->on("accounts", [](const QJsonObject &) { return QJsonObject{{"error", "accounts.toml: expected a table"}}; });
        boot();
        QTRY_COMPARE(app->note(), QString("accounts.toml: expected a table"));
        QVERIFY(app->noteError());
        QVERIFY(c->connected());
        QCOMPARE(app->statusText(), QString());   // connected: not "Mail service is not running"
        QVERIFY(!app->statusError());
    }

    void startup_folders_failure_syncs_after_reconnect()
    {
        auto fail = std::make_shared<bool>(true);
        d->on("folders", [this, fail](const QJsonObject &r) {
            if (std::exchange(*fail, false))
                return QJsonObject{{"error", "database is locked"}};
            const QString a = r.value("account").toString();
            return QJsonObject{{"result", folders.value(a.isEmpty() ? QStringLiteral("unified") : a)}};
        });
        boot(59);
        QTRY_COMPARE(app->note(), QString("database is locked"));
        barrier();
        QVERIFY(sent("list").isEmpty());
        QVERIFY(sent("sync").isEmpty());
        d->stop();
        QTRY_VERIFY(!c->connected());
        QVERIFY(d->start());
        QTRY_COMPARE(sent("sync").size(), 1);
        QCOMPARE(body(sent("list").at(0)), inboxList(0, 50));
        QTRY_COMPARE(list->cursor(), 1);   // `--open 59` still honoured
    }

    void search_error_sets_note()
    {
        d->on("search", [](const QJsonObject &) { return QJsonObject{{"error", "search index is broken"}}; });
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
        app->setQuery("ozon");
        QTRY_COMPARE(app->note(), QString("search index is broken"));
        QVERIFY(app->noteError());
    }

    void switch_drops_old_view_reply()
    {
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
        QTRY_COMPARE(letter->id(), 57);
        QJsonArray trash;
        for (int i = 0; i < 3; ++i) {
            QJsonObject r = rows.at(i).toObject();
            r["id"] = 1001 + i;
            r["seen"] = false;
            trash << r;
        }
        d->on("list", [this, trash](const QJsonObject &r) {
            if (r.value("role").toString() == "trash")
                return QJsonObject{{"result", trash}, {"delay", 100}};   // still on its way when the view switches
            return QJsonObject{{"result", slice(r)}};
        });
        d->on("folders", [this](const QJsonObject &r) {
            const QString a = r.value("account").toString();
            QJsonObject o{{"result", folders.value(a.isEmpty() ? QStringLiteral("unified") : a)}};
            if (a == "me@yandex.ru")
                o["delay"] = 200;   // after the old list, as maild answers in order
            return o;
        });
        QList<qint64> firstIds;   // the top row after each reset
        const auto conn = connect(list, &QAbstractItemModel::modelReset, this,
                                  [&] { firstIds << list->row(0).value("id").toInteger(); });
        app->pickFolder(3);    // Trash
        app->pickAccount(1);   // me@yandex.ru, before Trash's rows arrive
        QTRY_VERIFY(!firstIds.isEmpty());
        disconnect(conn);
        QCOMPARE(firstIds.first(), 60);   // the new view's rows, never Trash's
        QCOMPARE(body(sent("list").last()),
                 QJsonObject({{"cmd", "list"}, {"folder", 3}, {"unread_only", false}, {"offset", 0}, {"limit", 50}}));
        for (const QJsonObject &m : sent("message"))
            QVERIFY(m.value("message").toInteger() < 1000);
    }

    void removed_folder_falls_back_to_inbox()
    {
        accounts = QJsonArray{accounts.at(0)};
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
        app->pickFolder(4);   // Work
        QTRY_COMPARE(body(sent("list").last()).value("folder").toInt(), 7);
        QJsonArray gmail = folders.value("me@gmail.com").toArray();
        gmail.removeAt(4);
        folders["me@gmail.com"] = gmail;
        d->emitEvent({{"event", "changed"}, {"folders", true}, {"ids", QJsonArray()}});
        QTRY_COMPARE(app->folderIndex(), 0);
        QTRY_COMPARE(body(sent("list").last()),
                     QJsonObject({{"cmd", "list"}, {"folder", 1}, {"unread_only", false}, {"offset", 0}, {"limit", 50}}));
    }
};

QTEST_MAIN(TstAppData)
#include "tst_app_data.moc"
