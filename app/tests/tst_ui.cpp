#define QTEST_THROW_ON_FAIL   // helpers below verify too
#include "AppHarness.h"
#include "Session.h"
#include <QPointer>
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
        h.d->emitEvent({{"event", "changed"}, {"folders", false}, {"ids", QJsonArray()}});   // the rows again: a model reset
        QTRY_VERIFY(h.sent("list").size() > lists);
        QTRY_COMPARE(s->list.cursor(), 40);
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
        start();   // before auto-read (1 s) takes row 3's mark away
        QQuickItem *unread = itemAt(item("mailList"), 3), *seen = itemAt(item("mailList"), 0);
        QVERIFY(unread->findChild<QQuickItem *>("unreadBar")->isVisible());
        QVERIFY(!seen->findChild<QQuickItem *>("unreadBar")->isVisible());
    }
};

QTEST_MAIN(TstUi)
#include "tst_ui.moc"
