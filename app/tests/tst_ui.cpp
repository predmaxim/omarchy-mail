#define QTEST_THROW_ON_FAIL   // helpers below verify too
#include "AppHarness.h"
#include "Session.h"
#include <QPointer>
#include <QScopeGuard>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

using namespace om;
using namespace om::test;

// The window as main() builds it (Session), offscreen, on FakeMaild serving the fixtures.
class TstUi : public QObject {
    Q_OBJECT
    AppHarness h;   // its FakeMaild and fixtures only
    QTemporaryDir *dir = nullptr;
    Session *s = nullptr;
    QQmlApplicationEngine *engine = nullptr;
    QQuickWindow *win = nullptr;

    QQuickItem *item(const char *name) const
    {
        auto *i = win->findChild<QQuickItem *>(QLatin1String(name));
        QVERIFY2(i, name);
        return i;
    }
    static QQuickItem *itemAt(QQuickItem *view, int i)   // ListView and Repeater
    {
        QQuickItem *out = nullptr;
        const bool repeater = view->metaObject()->indexOfMethod("itemAt(int)") >= 0;
        QMetaObject::invokeMethod(view, repeater ? "itemAt" : "itemAtIndex", Q_RETURN_ARG(QQuickItem *, out), Q_ARG(int, i));
        QVERIFY(out);
        return out;
    }
    // Message `id` as message.json: two attachments and an inline logo.
    static QJsonObject letter(qint64 id)
    {
        QJsonObject f = fixture("message.json").object();
        QJsonObject sum = f.value("summary").toObject();
        sum.insert("id", id);
        f.insert("summary", sum);
        f.insert("inline", QJsonObject{{"logo@x", QStringLiteral(OM_SOURCE_DIR "/tests/fixtures/logo.png")}});
        return f;
    }
    void type(const char *text)   // QTest::keyClicks is for widgets
    {
        for (; *text; ++text)
            QTest::keyClick(win, *text);
    }
    // The first page is in.
    void start()
    {
        s->start(0);
        QTRY_VERIFY(item("mailList")->property("count").toInt() >= 50);
    }
    // The open letter has its attachments and its body laid out.
    void letterShown()
    {
        QTRY_COMPARE(item("attachments")->property("count").toInt(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(item("letterView")->property("contentHeight").toInt() > 0, 10000);
    }

private Q_SLOTS:
    void init()
    {
        QTest::failOnWarning();
        h.serve();
        h.d->on("message", [](const QJsonObject &r) { return QJsonObject{{"result", letter(r.value("message").toInteger())}}; });
        dir = new QTemporaryDir(scratch());
        s = new Session(h.d->path(), dir->filePath("drafts"), dir->filePath("img"));
        engine = new QQmlApplicationEngine;
        s->load(*engine);
        QCOMPARE(engine->rootObjects().size(), 1);
        win = qobject_cast<QQuickWindow *>(engine->rootObjects().at(0));
        QVERIFY(win);
        QVERIFY(QTest::qWaitForWindowActive(win));
    }
    void cleanup()
    {
        delete engine;   // before the session: QML holds its objects
        delete s;
        delete dir;
        delete h.d;
        h.d = nullptr;
    }

    void window_loads_and_paints()
    {
        start();
        QTRY_VERIFY(item("accountPicker")->isVisible());   // two mailboxes
        QCOMPARE(win->grabWindow().pixelColor(0, 0), s->theme.background());
    }
    void keys_reach_app()
    {
        start();
        QTest::keyClick(win, Qt::Key_D);
        QTRY_COMPARE(h.sent("act").size(), 1);
        QCOMPARE(h.sent("act").at(0).value("op").toString(), QString("delete"));
    }
    void slash_focuses_search_and_typing_searches()
    {
        start();
        QQuickItem *field = item("searchField");
        QTest::keyClick(win, Qt::Key_Slash);
        QVERIFY(field->hasActiveFocus());
        QTest::keyClick(win, 'o');
        QTest::keyClick(win, 'z');
        QTRY_COMPARE(h.sent("search").size(), 1);
        QCOMPARE(body(h.sent("search").at(0)), QJsonObject({{"cmd", "search"}, {"q", "oz"}}));
        QTest::keyClick(win, Qt::Key_Up);   // the field drops it: not the window's
        QCOMPARE(s->nav.row(), int(Row::Search));
        QTest::keyClick(win, Qt::Key_Escape);
        QCOMPARE(s->app.query(), QString());
        QCOMPARE(field->property("text").toString(), QString());
    }
    void attachments_above_body()
    {
        // made-up rows for the shot: flagged, unsent, unread (opened first; row 3 stays unread after auto-read)
        const auto set = [this](int i, const QJsonObject &fields) {
            QJsonObject r = h.rows.at(i).toObject();
            for (auto it = fields.begin(); it != fields.end(); ++it)
                r.insert(it.key(), it.value());
            h.rows[i] = r;
        };
        set(0, {{"flagged", true}});
        set(1, {{"queued", true}, {"failed", true}});
        set(2, {{"seen", false}, {"has_attachments", true}});
        start();
        letterShown();
        QQuickItem *chip = itemAt(item("attachments"), 0), *view = item("letterView");
        QVERIFY(chip->isVisible());
        QVERIFY(chip->mapToScene(QPointF(0, chip->height())).y() <= view->mapToScene(QPointF(0, 0)).y());
        if (const QString shot = qEnvironmentVariable("OM_SHOT"); !shot.isEmpty())
            QVERIFY(win->grabWindow().save(shot));
    }
    void attachments_survive_relayout()
    {
        start();
        letterShown();
        const QPointer<QQuickItem> chip = itemAt(item("attachments"), 0);
        QSignalSpy docs(&s->letter, &Letter::documentChanged);
        win->resize(1000, 800);
        QTRY_VERIFY(docs.size() >= 1);
        QVERIFY(chip);
        QCOMPARE(itemAt(item("attachments"), 0), chip.data());
    }
    void page_down_on_the_list_scrolls_the_letter()
    {
        h.d->on("message", [](const QJsonObject &r) {
            QJsonObject f = letter(r.value("message").toInteger());
            f.insert("html", "<div style=\"height:5000px\">tall</div>");
            return QJsonObject{{"result", f}};
        });
        start();
        QQuickItem *view = item("letterView");
        QTRY_VERIFY_WITH_TIMEOUT(view->property("contentHeight").toInt() > view->height(), 10000);
        QVERIFY(s->app.letterAtTop());
        QTest::keyClick(win, Qt::Key_PageDown);
        QVERIFY(view->property("scrollY").toReal() > 0);
        QVERIFY(!s->app.letterAtTop());
        QTest::keyClick(win, Qt::Key_PageUp);
        QCOMPARE(view->property("scrollY").toReal(), 0.0);
    }
    void open_picker_keeps_the_keys_then_gives_them_back()
    {
        start();
        const int cursor = s->list.cursor();
        Q_EMIT s->app.openPicker("folder");
        QTest::keyClick(win, Qt::Key_Down);
        QCOMPARE(s->list.cursor(), cursor);   // the picker's list moved, not the mail list
        QTest::keyClick(win, Qt::Key_Return);
        QTRY_COMPARE(body(h.sent("list").last()), QJsonObject({{"cmd", "list"}, {"role", "flagged"}, {"unread_only", false},
                                                              {"offset", 0}, {"limit", 50}}));
        QTest::keyClick(win, Qt::Key_D);
        QTRY_COMPARE(h.sent("act").size(), 1);
    }
    void cursor_row_stays_in_view_after_reload()
    {
        start();
        s->list.setCursor(40);
        QQuickItem *view = item("mailList");
        const auto inView = [&] {
            QQuickItem *r = itemAt(view, 40);
            const qreal y = r->y() - view->property("contentY").toReal();
            return y >= 0 && y + r->height() <= view->height();
        };
        QTRY_VERIFY(inView());
        const int lists = h.sent("list").size();
        QSignalSpy resets(&s->list, &QAbstractItemModel::modelReset);
        h.d->emitEvent({{"event", "changed"}, {"folders", false}, {"ids", QJsonArray()}});   // the rows again
        QTRY_VERIFY(h.sent("list").size() > lists);
        QTRY_COMPARE(resets.size(), 1);
        QCOMPARE(s->list.cursor(), 40);
        QVERIFY(inView());
    }
    void single_account_hides_account_picker()
    {
        h.accounts = QJsonArray{h.accounts.at(0)};
        start();
        QVERIFY(!item("accountPicker")->isVisible());
        QCOMPARE(item("folderPicker")->width(), item("mailList")->width());   // the pickers' line and the list share the inset
    }
    void unread_row_has_accent_bar()
    {
        start();
        QQuickItem *view = item("mailList");
        QVERIFY(!itemAt(view, 0)->findChild<QQuickItem *>("unreadBar")->isVisible());
        QVERIFY(s->list.cursor() != 10);   // unread, and not the open letter: auto-read never takes its mark
        QMetaObject::invokeMethod(view, "positionViewAtIndex", Q_ARG(int, 10), Q_ARG(int, 4 /* ListView.Contain */));
        QVERIFY(itemAt(view, 10)->findChild<QQuickItem *>("unreadBar")->isVisible());
    }
    void paddings_follow_a_live_base_size()
    {
        start();
        QQuickItem *bar = itemAt(item("mailList"), 0)->findChild<QQuickItem *>("unreadBar");
        QCOMPARE(bar->width(), qreal(s->theme.space(3)));
        const QByteArray config = qgetenv("XDG_CONFIG_HOME");
        const auto restore = qScopeGuard([config] { qputenv("XDG_CONFIG_HOME", config); });
        qputenv("XDG_CONFIG_HOME", dir->filePath("config").toUtf8());   // not the shared test home
        QDir().mkpath(dir->filePath("config/omarchy"));
        QFile toml(dir->filePath("config/omarchy/shell.toml"));
        QVERIFY(toml.open(QIODevice::WriteOnly));
        toml.write(QByteArray("[font]\nbase-size = ") + QByteArray::number(s->theme.baseSize() + 8) + "\n");
        toml.close();
        const int before = s->theme.space(3);
        s->theme.reload();
        QVERIFY(s->theme.space(3) > before);
        QCOMPARE(bar->width(), qreal(s->theme.space(3)));
    }

    // ---- the mailboxes page ----

    void gear_opens_settings_esc_returns()
    {
        h.d->on("status", [](const QJsonObject &) {   // for the shot: a refused login and an offline mailbox
            return QJsonObject{{"result", QJsonObject{{"accounts", QJsonArray{
                QJsonObject{{"email", "me@gmail.com"}, {"status", QJsonObject{{"offline", "no route"}}}, {"queued", 0}},
                QJsonObject{{"email", "me@yandex.ru"}, {"status", "auth_failed"}, {"queued", 0}}}}}}};
        });
        start();
        QVERIFY(!item("settingsPage")->isVisible());
        s->app.pressAction(4);   // the gear
        QTRY_VERIFY(item("settingsPage")->isVisible());
        for (const char *hidden : {"mailList", "searchField", "letterView", "folderPicker"})
            QVERIFY2(!item(hidden)->isVisible(), hidden);
        QTRY_COMPARE(item("accountRows")->property("count").toInt(), 2);
        QTRY_VERIFY(item("accountRows")->hasActiveFocus());
        if (const QString shot = qEnvironmentVariable("OM_SHOT_SETTINGS"); !shot.isEmpty())
            QVERIFY(win->grabWindow().save(shot));
        QTest::keyClick(win, Qt::Key_Escape);
        QCOMPARE(s->app.page(), QString("main"));
        QVERIFY(item("mailList")->isVisible());
        QVERIFY(!item("settingsPage")->isVisible());
        QTest::keyClick(win, Qt::Key_D);   // the window has the keys again
        QTRY_COMPARE(h.sent("act").size(), 1);
    }
    void no_accounts_shows_settings()
    {
        h.accounts = {};
        s->start(0);
        QTRY_VERIFY(item("settingsPage")->isVisible());
        QVERIFY(!item("mailList")->isVisible());
        QCOMPARE(item("accountRows")->property("count").toInt(), 0);
        QTRY_VERIFY(item("providerRow")->hasActiveFocus());   // the first mailbox: the form has the keys
    }
    void settings_keys_rows_and_form()
    {
        h.d->on("status", [](const QJsonObject &) {
            return QJsonObject{{"result", QJsonObject{{"accounts", QJsonArray{
                QJsonObject{{"email", "me@gmail.com"}, {"status", "online"}, {"queued", 0}},
                QJsonObject{{"email", "me@yandex.ru"}, {"status", "auth_failed"}, {"queued", 0}}}}}}};
        });
        for (const char *cmd : {"account_remove", "account_update"})
            h.d->on(cmd, [](const QJsonObject &) { return QJsonObject{}; });
        h.d->on("account_add", [](const QJsonObject &) { return QJsonObject{{"error", "AUTHENTICATIONFAILED"}}; });
        start();
        s->app.pressAction(4);
        QQuickItem *rows = item("accountRows");
        QTRY_VERIFY(rows->hasActiveFocus());
        QTRY_VERIFY(s->app.accounts().value(1).toMap().value("authFailed").toBool());
        const int cursor = s->list.cursor();

        QTest::keyClick(win, Qt::Key_Down);
        QCOMPARE(rows->property("currentIndex").toInt(), 1);
        QCOMPARE(s->list.cursor(), cursor);   // the page's own cursor, not the mail list's
        QTest::keyClick(win, Qt::Key_Return);   // a refused login: try again
        QTRY_COMPARE(body(h.sent("sync").last()), QJsonObject({{"cmd", "sync"}, {"account", "me@yandex.ru"}}));

        QTest::keyClick(win, Qt::Key_Up);
        QTest::keyClick(win, Qt::Key_Return);   // the sender name
        QQuickItem *rename = itemAt(rows, 0)->findChild<QQuickItem *>("renameField");
        QVERIFY(rename && rename->hasActiveFocus());
        QCOMPARE(rename->property("text").toString(), QString("Max"));
        QTest::keyClick(win, Qt::Key_End);
        type(" P");
        QTest::keyClick(win, Qt::Key_Escape);   // cancelled, still on the page
        QCOMPARE(win->activeFocusItem(), rows);
        QCOMPARE(s->app.page(), QString("settings"));
        QTest::keyClick(win, Qt::Key_Return);
        QTest::keyClick(win, Qt::Key_End);
        type(" P");
        QTest::keyClick(win, Qt::Key_Return);
        QTRY_COMPARE(h.sent("account_update").size(), 1);
        QCOMPARE(body(h.sent("account_update").at(0)),
                 QJsonObject({{"cmd", "account_update"}, {"email", "me@gmail.com"}, {"name", "Max P"}}));
        QCOMPARE(win->activeFocusItem(), rows);

        QTest::keyClick(win, Qt::Key_D);
        QTest::keyClick(win, Qt::Key_D);
        QTRY_COMPARE(h.sent("account_remove").size(), 1);
        QCOMPARE(h.sent("account_remove").at(0).value("email").toString(), QString("me@gmail.com"));
        QVERIFY(h.sent("act").isEmpty());

        QTest::keyClick(win, Qt::Key_Down);
        QTest::keyClick(win, Qt::Key_Down);   // past the last mailbox: the providers
        QVERIFY(item("providerRow")->hasActiveFocus());
        QVERIFY(!item("imapHostField")->isVisible());
        QTest::keyClick(win, Qt::Key_Right);
        QTest::keyClick(win, Qt::Key_Right);   // Other IMAP
        QVERIFY(item("imapHostField")->isVisible());
        QTest::keyClick(win, Qt::Key_Left);
        QTest::keyClick(win, Qt::Key_Left);   // Gmail
        QVERIFY(!item("imapHostField")->isVisible());
        QTest::keyClick(win, Qt::Key_Down);
        QVERIFY(item("emailField")->hasActiveFocus());
        type("new@gmail.com");
        QTest::keyClick(win, Qt::Key_Return);   // the next field
        QVERIFY(item("nameField")->hasActiveFocus());
        QTest::keyClick(win, Qt::Key_Down);
        QVERIFY(item("passwordField")->hasActiveFocus());
        type("secret");
        QTest::keyClick(win, Qt::Key_Return);   // the last field: Add
        QTRY_COMPARE(h.sent("account_add").size(), 1);
        QCOMPARE(body(h.sent("account_add").at(0)), QJsonObject({{"cmd", "account_add"}, {"provider", "gmail"},
                                                                {"email", "new@gmail.com"}, {"name", ""}, {"password", "secret"}}));
        QCOMPARE(item("passwordField")->property("text").toString(), QString());   // cleared after every attempt
        QTRY_COMPARE(item("addError")->property("text").toString(), QString("AUTHENTICATIONFAILED"));
        QCOMPARE(item("emailField")->property("text").toString(), QString("new@gmail.com"));
        QTest::keyClick(win, Qt::Key_Down);
        QVERIFY(item("helpLink")->hasActiveFocus());
        QTest::keyClick(win, Qt::Key_Down);
        QVERIFY(item("addButton")->hasActiveFocus());
        QTest::keyClick(win, Qt::Key_Escape);
        QCOMPARE(s->app.page(), QString("main"));
    }
};

QTEST_MAIN(TstUi)
#include "tst_ui.moc"
