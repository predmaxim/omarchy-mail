#define QTEST_THROW_ON_FAIL   // helpers below verify too
#include "AppHarness.h"
#include "Session.h"
#include <QClipboard>
#include <QGuiApplication>
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
    QTemporaryDir *bin = nullptr;   // gsettings: GNOME's text-scaling-factor is $OM_TEST_TEXT_SCALE, else 1

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

    // XDG_CONFIG_HOME in the test's own dir until the scope ends: window.json and shell.toml stay out of the shared home.
    [[nodiscard]] auto ownConfig()
    {
        const QByteArray config = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", dir->filePath("config").toUtf8());
        return qScopeGuard([config] { qputenv("XDG_CONFIG_HOME", config); });
    }

private Q_SLOTS:
    void initTestCase()
    {
        bin = new QTemporaryDir(scratch());
        QFile f(bin->filePath("gsettings"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\necho \"${OM_TEST_TEXT_SCALE:-1}\"\n");
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", bin->path().toUtf8() + ':' + qgetenv("PATH"));
    }
    void cleanupTestCase() { delete bin; }
    void init()
    {
        QTest::failOnWarning();
        h.serve();
        h.d->on("message", [](const QJsonObject &r) { return QJsonObject{{"result", letter(r.value("message").toInteger())}}; });
        dir = new QTemporaryDir(scratch());
        // a zoom some test left in the shared home would be every later window's
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omarchy-mail/window.json");
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
    void wheel_browsing_stays_on_a_reload()
    {
        start();
        QTRY_COMPARE(s->list.cursor(), 3);
        QQuickItem *view = item("mailList");
        view->setProperty("contentY", 2000);   // the wheel: the cursor row out of sight
        const qreal y = view->property("contentY").toReal();
        QVERIFY(y > 1000);
        const int lists = h.sent("list").size();
        QSignalSpy resets(&s->list, &QAbstractItemModel::modelReset);
        h.d->emitEvent({{"event", "changed"}, {"folders", false}, {"ids", QJsonArray()}});
        QTRY_VERIFY(h.sent("list").size() > lists);
        QTRY_COMPARE(resets.size(), 1);
        QCOMPARE(s->list.cursor(), 3);
        QCOMPARE(view->property("contentY").toReal(), y);
    }
    void picker_highlight_survives_new_counts()
    {
        start();
        Q_EMIT s->app.openPicker("folder");
        QTest::keyClick(win, Qt::Key_Down);
        QTest::keyClick(win, Qt::Key_Down);   // Sent
        QJsonArray unified = h.folders.value("unified").toArray();
        QJsonObject inbox = unified.at(0).toObject();
        inbox["unread"] = 99;
        unified[0] = inbox;
        h.folders["unified"] = unified;
        h.d->emitEvent({{"event", "changed"}, {"folders", true}, {"ids", QJsonArray()}});
        QTRY_COMPARE(s->app.folderChoices().at(0).toMap().value("unread").toInt(), 99);
        QTest::keyClick(win, Qt::Key_Return);
        QTRY_COMPARE(s->app.folderIndex(), 2);
        QCOMPARE(body(h.sent("list").last()).value("role").toString(), QString("sent"));
    }
    void click_on_the_letter_takes_the_keys_from_search()
    {
        start();
        letterShown();
        QQuickItem *field = item("searchField"), *view = item("letterView");
        QTest::keyClick(win, Qt::Key_Slash);
        QVERIFY(field->hasActiveFocus());
        QGuiApplication::clipboard()->setText("before");
        const int m = s->theme.space(18);
        const auto at = [&](int x) { return view->mapToScene(QPointF(m + x, 8)).toPoint(); };
        QTest::mousePress(win, Qt::LeftButton, {}, at(2));   // a drag over the first line selects some text
        QTest::mouseMove(win, at(60));
        QTest::mouseMove(win, at(120));
        QTest::mouseRelease(win, Qt::LeftButton, {}, at(120));
        QVERIFY(!field->hasActiveFocus());
        QString selected;
        QMetaObject::invokeMethod(view, "selectedText", Q_RETURN_ARG(QString, selected));
        QVERIFY(!selected.isEmpty());
        QTest::keyClick(win, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), selected);
    }
    void single_account_hides_account_picker()
    {
        h.accounts = QJsonArray{h.accounts.at(0)};
        start();
        QVERIFY(!item("accountPicker")->isVisible());
        QCOMPARE(item("folderPicker")->width(), item("mailList")->width());   // the pickers' line and the list share the inset
    }
    void c_opens_compose_with_focus_in_to()
    {
        start();
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        QVERIFY(item("composePane")->isVisible());
        QVERIFY(!item("mailList")->isEnabled());
    }
    void ctrl_enter_sends()
    {
        start();
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        type("ivan@example.org");
        item("composeSubject")->forceActiveFocus();
        type("Hi");
        item("composeBody")->forceActiveFocus();
        type("x");
        QTest::keyClick(win, Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(h.sent("send").size(), 1);
        const QJsonObject r = h.sent("send").at(0);
        QCOMPARE(r.value("to").toArray(), QJsonArray({QJsonObject{{"name", ""}, {"addr", "ivan@example.org"}}}));
        QCOMPARE(r.value("subject").toString(), QString("Hi"));
        QCOMPARE(r.value("text").toString(), QString("x"));
        QTRY_VERIFY(!s->compose.isOpen());
        QVERIFY(!item("composePane")->isVisible());
    }
    void esc_keeps_draft()
    {
        start();
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        item("composeSubject")->forceActiveFocus();
        type("Topic");
        QTest::keyClick(win, Qt::Key_Escape);
        QTRY_VERIFY(!s->compose.isOpen());
        QVERIFY(item("mailList")->isEnabled());
        QTest::keyClick(win, Qt::Key_C);   // the focus went back to the window: its keys work
        QTRY_VERIFY(s->compose.isOpen());
        QCOMPARE(item("composeSubject")->property("text").toString(), QString("Topic"));
    }
    void reply_cursor_at_start()
    {
        start();
        letterShown();
        QTest::keyClick(win, Qt::Key_R);
        QTRY_VERIFY(s->compose.isOpen());
        QQuickItem *b = item("composeBody");
        QTRY_VERIFY(b->hasActiveFocus());
        QCOMPARE(b->property("cursorPosition").toInt(), 0);
        QVERIFY(b->property("text").toString().contains("wrote:"));
    }
    void to_suggestions_pick_only_a_highlighted_row()
    {
        h.d->on("contacts", [](const QJsonObject &) {
            return QJsonObject{{"result", QJsonArray{QJsonObject{{"name", "Pred"}, {"addr", "predmax@gmail.com"}},
                                                     QJsonObject{{"name", ""}, {"addr", "max@gmail.org"}}}}};
        });
        start();
        QTest::keyClick(win, Qt::Key_C);
        QQuickItem *to = item("composeTo"), *pane = item("composePane");
        QTRY_VERIFY(to->hasActiveFocus());
        const auto highlighted = [pane] { return pane->property("suggestion").toInt(); };
        type("max@gmail.com");
        QTRY_COMPARE(s->compose.suggestions().size(), 2);
        QCOMPARE(highlighted(), -1);
        QQuickItem *frame = to->parentItem()->parentItem();   // the framed line
        const auto y = [](QQuickItem *i) { return i->mapToScene(QPointF(0, 0)).y(); };
        QTRY_VERIFY(y(itemAt(item("composeSuggestions"), 0)) >= y(frame) + frame->height());   // under To, not over the title
        QQuickItem *rows = item("composeSuggestions")->parentItem();
        QVERIFY(rows->isVisible());
        QTest::keyClick(win, Qt::Key_Tab);   // nothing highlighted: the address as typed, the keys move on
        QCOMPARE(s->compose.to(), QString("max@gmail.com"));
        QVERIFY(item("composeSubject")->hasActiveFocus());
        QVERIFY(!rows->isVisible());   // not over Subject: no click there puts a contact into To
        QTest::keyClick(win, Qt::Key_Backtab, Qt::ShiftModifier);
        QVERIFY(to->hasActiveFocus());
        QVERIFY(rows->isVisible());
        QTest::keyClick(win, Qt::Key_Return);   // nor Enter
        QCOMPARE(s->compose.to(), QString("max@gmail.com"));
        QVERIFY(to->hasActiveFocus());

        QTest::keyClick(win, Qt::Key_Down);
        QCOMPARE(highlighted(), 0);
        type("x");   // typing on: the old rows' highlight goes
        QCOMPARE(highlighted(), -1);
        QTest::keyClick(win, Qt::Key_Backspace);
        QTRY_COMPARE(h.sent("contacts").last().value("prefix").toString(), QString("max@gmail.com"));
        for (int i = 0; i < 3; ++i)
            QTest::keyClick(win, Qt::Key_Down);
        QCOMPARE(highlighted(), 1);   // the last row
        for (int i = 0; i < 3; ++i)
            QTest::keyClick(win, Qt::Key_Up);
        QCOMPARE(highlighted(), 0);   // the first
        QTest::keyClick(win, Qt::Key_Return);
        QCOMPARE(s->compose.to(), QString("Pred <predmax@gmail.com>, "));
        QVERIFY(s->compose.suggestions().isEmpty());

        type("ma");
        QTRY_COMPARE(s->compose.suggestions().size(), 2);
        QCOMPARE(highlighted(), -1);
        QQuickItem *row = itemAt(item("composeSuggestions"), 1);
        QTest::mouseClick(win, Qt::LeftButton, {}, row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
        QCOMPARE(s->compose.to(), QString("Pred <predmax@gmail.com>, max@gmail.org, "));
        QVERIFY(to->hasActiveFocus());   // a click on a row leaves the keys in To
    }
    void late_suggestions_stay_hidden_once_to_is_left()
    {
        h.d->on("contacts", [](const QJsonObject &) {
            return QJsonObject{{"result", QJsonArray{QJsonObject{{"name", "Pred"}, {"addr", "predmax@gmail.com"}}}},
                               {"delay", 200}};
        });
        start();
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        type("max@gmail.com");
        QTest::keyClick(win, Qt::Key_Tab);   // before the contacts reply
        QVERIFY(item("composeSubject")->hasActiveFocus());
        QVERIFY(s->compose.suggestions().isEmpty());
        QTRY_COMPARE(s->compose.suggestions().size(), 1);
        QVERIFY(!item("composeSuggestions")->parentItem()->isVisible());
        type("Hi");
        QCOMPARE(s->compose.subject(), QString("Hi"));
        QCOMPARE(s->compose.to(), QString("max@gmail.com"));
    }
    void paste_image_in_body()
    {
        start();
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        item("composeBody")->forceActiveFocus();
        QImage img(8, 8, QImage::Format_RGB32);
        img.fill(Qt::red);
        QGuiApplication::clipboard()->setImage(img);
        QTest::keyClick(win, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(item("composeAttachments")->property("count").toInt(), 1);
        QCOMPARE(item("composeBody")->property("text").toString(), QString());
    }
    void attachments_below_subject()
    {
        start();
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        QFile f(dir->filePath("note.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hi");
        f.close();
        s->compose.addFiles({QUrl::fromLocalFile(f.fileName())});
        QTRY_COMPARE(item("composeAttachments")->property("count").toInt(), 1);
        QQuickItem *chip = itemAt(item("composeAttachments"), 0);
        QQuickItem *frame = item("composeSubject")->parentItem()->parentItem();   // the framed line
        const auto y = [](QQuickItem *i) { return i->mapToScene(QPointF(0, 0)).y(); };
        QTRY_VERIFY(y(chip) >= y(frame) + frame->height());
        QTRY_VERIFY(y(chip) + chip->height() <= y(item("composeBody")));
        if (const QString shot = qEnvironmentVariable("OM_SHOT"); !shot.isEmpty())
            QVERIFY(win->grabWindow().save(shot));
    }
    void tab_chain_reaches_every_control()
    {
        start();
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        QFile f(dir->filePath("note.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hi");
        f.close();
        s->compose.addFiles({QUrl::fromLocalFile(f.fileName())});
        QTRY_COMPARE(item("composeAttachments")->property("count").toInt(), 1);
        QQuickItem *plaque = itemAt(item("composeAttachments"), 0);
        const QList<QQuickItem *> order{item("composeSubject"), plaque, item("composeBody"), item("composeFrom"), item("composeTo")};
        for (QQuickItem *next : order) {
            QTest::keyClick(win, Qt::Key_Tab);
            QVERIFY(next->hasActiveFocus());
        }
        QTest::keyClick(win, Qt::Key_Backtab, Qt::ShiftModifier);   // back: From <- To
        QVERIFY(item("composeFrom")->hasActiveFocus());
    }
    void tab_in_body_inserts_nothing()
    {
        start();
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        item("composeBody")->forceActiveFocus();
        QTest::keyClick(win, Qt::Key_Tab);
        QCOMPARE(item("composeBody")->property("text").toString(), QString());
        QVERIFY(!item("composeBody")->hasActiveFocus());
    }
    void del_on_focused_plaque_removes_it()
    {
        start();
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        QFile f(dir->filePath("note.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hi");
        f.close();
        s->compose.addFiles({QUrl::fromLocalFile(f.fileName())});
        QTRY_COMPARE(item("composeAttachments")->property("count").toInt(), 1);
        QTest::keyClick(win, Qt::Key_Tab);
        QTest::keyClick(win, Qt::Key_Tab);   // Subject, then the plaque
        QVERIFY(itemAt(item("composeAttachments"), 0)->hasActiveFocus());
        QTest::keyClick(win, Qt::Key_Delete);
        QTRY_COMPARE(item("composeAttachments")->property("count").toInt(), 0);
        QTRY_VERIFY(item("composeBody")->hasActiveFocus());
    }
    void from_picker_opens_from_the_keyboard()
    {
        start();
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        QTest::keyClick(win, Qt::Key_Backtab, Qt::ShiftModifier);
        QVERIFY(item("composeFrom")->hasActiveFocus());
        QTest::keyClick(win, Qt::Key_Return);
        QTest::keyClick(win, Qt::Key_Down);
        QTest::keyClick(win, Qt::Key_Return);
        QTRY_COMPARE(s->compose.account(), s->app.accounts().at(1).toMap().value("email").toString());
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
    void empty_subject_shows_a_dim_placeholder()
    {
        const auto subject = [this](int i, const QString &s) {
            QJsonObject r = h.rows.at(i).toObject();
            r.insert("subject", s);
            h.rows[i] = r;
        };
        subject(0, "");
        subject(1, " \t ");
        h.d->on("message", [](const QJsonObject &r) {
            QJsonObject f = letter(r.value("message").toInteger()), sum = f.value("summary").toObject();
            sum.insert("subject", "");
            f.insert("summary", sum);
            return QJsonObject{{"result", f}};
        });
        QQuickItem *heading = item("letterSubject");
        QVERIFY(!heading->isVisible());   // no letter open yet: no heading at all
        start();
        QQuickItem *view = item("mailList");
        for (int i : {0, 1}) {
            QQuickItem *line = itemAt(view, i)->findChild<QQuickItem *>("subject");
            QCOMPARE(line->property("text").toString(), QString("(no subject)"));
            QCOMPARE(line->property("color").value<QColor>().rgba(), s->theme.dim().rgba());
        }
        QQuickItem *real = itemAt(view, 2)->findChild<QQuickItem *>("subject");
        QCOMPARE(real->property("text").toString(), h.rows.at(2).toObject().value("subject").toString());
        QCOMPARE(real->property("color").value<QColor>().rgba(), s->theme.foreground().rgba());
        letterShown();
        QVERIFY(heading->isVisible());
        QCOMPARE(heading->property("text").toString(), QString("(no subject)"));
        QCOMPARE(heading->property("color").value<QColor>().rgba(), s->theme.dim().rgba());
        QCOMPARE(s->letter.subject(), QString());   // the placeholder is the window's only
    }
    void letter_without_text_says_so()
    {
        auto mode = std::make_shared<int>(0);   // 0 text, 1 no text, 2 no text and no attachments
        h.d->on("message", [mode](const QJsonObject &r) {
            QJsonObject f = letter(r.value("message").toInteger());
            if (*mode > 0) {
                f.insert("html", "");
                f.insert("text", " \n ");
            }
            if (*mode > 1)
                f.insert("attachments", QJsonArray());
            return QJsonObject{{"result", f}};
        });
        start();
        letterShown();
        QQuickItem *note = item("noText"), *view = item("letterView");
        QVERIFY(!note->isVisible());
        *mode = 1;
        s->letter.reload();
        QTRY_VERIFY_WITH_TIMEOUT(note->isVisible(), 10000);
        QCOMPARE(note->property("text").toString(), QString("No content"));
        QCOMPARE(note->property("color").value<QColor>().rgba(), s->theme.dim().rgba());
        const QPointF centre = note->mapToScene(QPointF(note->width() / 2, note->height() / 2)),
                      middle = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
        QVERIFY(qAbs(centre.x() - middle.x()) <= 1 && qAbs(centre.y() - middle.y()) <= 1);
        *mode = 2;
        s->letter.reload();
        QTRY_COMPARE_WITH_TIMEOUT(note->property("text").toString(), QString("No content"), 10000);
        QVERIFY(note->isVisible());
        *mode = 0;
        s->letter.reload();
        QTRY_VERIFY_WITH_TIMEOUT(!note->isVisible(), 10000);
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

    void grey_lines_are_subtitle_size()   // the list's preview, place and date, the letter's "to: … · date"
    {
        start();
        letterShown();
        QQuickItem *row = itemAt(item("mailList"), 0);
        for (const char *name : {"rowPreview", "rowPlace", "rowDate"}) {
            auto *text = row->findChild<QQuickItem *>(name);
            QVERIFY2(text && text->isVisible(), name);
            QCOMPARE(text->property("font").value<QFont>().pixelSize(), s->theme.fontSubtitle());
        }
        QCOMPARE(item("letterMeta")->property("font").value<QFont>().pixelSize(), s->theme.fontSubtitle());
    }
    void letter_zoom_follows_gnome_and_is_remembered()
    {
        const auto config = ownConfig();
        const QByteArray path = qgetenv("PATH");
        const auto restore = qScopeGuard([path] {
            qputenv("PATH", path);
            qunsetenv("OM_TEST_TEXT_SCALE");
        });
        const auto zoom = [this] { return Session(h.d->path(), dir->filePath("drafts"), dir->filePath("img")).letter.zoom(); };
        qputenv("OM_TEST_TEXT_SCALE", "1.5");
        QCOMPARE(zoom(), 1.5);   // GNOME's text-scaling-factor
        qputenv("OM_TEST_TEXT_SCALE", "nonsense");
        QCOMPARE(zoom(), 1.0);
        qputenv("PATH", dir->filePath("nowhere").toUtf8());   // no gsettings at all
        QCOMPARE(zoom(), 1.0);
        qputenv("PATH", path);
        qputenv("OM_TEST_TEXT_SCALE", "1.5");
        Session(h.d->path(), dir->filePath("drafts"), dir->filePath("img")).letter.zoomBy(1);
        QCOMPARE(zoom(), 1.65);   // the reader's own, over GNOME's
        Session(h.d->path(), dir->filePath("drafts"), dir->filePath("img")).letter.zoomBy(0);
        QCOMPARE(zoom(), 1.5);   // follows GNOME again
    }
    void ctrl_plus_minus_zero_zoom_the_letter_only()
    {
        const auto config = ownConfig();
        start();
        letterShown();
        QQuickItem *view = item("letterView"), *row = itemAt(item("mailList"), 0);
        const int height = view->property("contentHeight").toInt();
        const qreal rowHeight = row->height();
        QSignalSpy docs(&s->letter, &Letter::documentChanged);
        QTest::keyClick(win, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(s->letter.zoom(), 1.1);
        QTRY_VERIFY_WITH_TIMEOUT(view->property("contentHeight").toInt() > height, 10000);
        QCOMPARE(row->height(), rowHeight);   // the list keeps Omarchy's size
        QTest::keyClick(win, Qt::Key_Slash);
        QVERIFY(item("searchField")->hasActiveFocus());
        QTest::keyClick(win, Qt::Key_Minus, Qt::ControlModifier);   // from the search field too
        QCOMPARE(s->letter.zoom(), 1.0);
        QCOMPARE(item("searchField")->property("text").toString(), QString());
        QTest::keyClick(win, Qt::Key_Escape);
        QTest::keyClick(win, Qt::Key_Equal, Qt::ControlModifier);
        QTest::keyClick(win, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(s->letter.zoom(), 1.0);
        QTest::keyClick(win, Qt::Key_C);
        QTRY_VERIFY(item("composeTo")->hasActiveFocus());
        QTest::keyClick(win, Qt::Key_Equal, Qt::ControlModifier);   // the form shows no letter
        QCOMPARE(s->letter.zoom(), 1.0);
        // saved while this test's config is set: the Session dies after it, in cleanup()
        QTRY_VERIFY([this] {
            QFile f(dir->filePath("config/omarchy-mail/window.json"));
            return f.open(QIODevice::ReadOnly) && !QJsonDocument::fromJson(f.readAll()).object().contains("letterZoom");
        }());
    }

    // OM_SHOT_ZOOM=<prefix>: made-up letters, <prefix>-100.png and <prefix>-136.png; <prefix>-fit.png: a 600 px
    // newsletter in a ~600 px pane at 136 %
    void letter_zoom_shots()
    {
        const QString shot = qEnvironmentVariable("OM_SHOT_ZOOM");
        if (shot.isEmpty())
            QSKIP("OM_SHOT_ZOOM not set");
        const auto newsletter = std::make_shared<bool>(false);
        h.d->on("message", [newsletter](const QJsonObject &r) {
            QJsonObject f = letter(r.value("message").toInteger());
            f.insert("html", *newsletter ? QStringLiteral(
                "<table width=\"100%\" bgcolor=\"#eeeeee\"><tr><td align=\"center\">"
                "<table width=\"600\" cellpadding=\"24\" bgcolor=\"#ffffff\" style=\"font-family:Arial\">"
                "<tr><td><img src=\"cid:logo@x\" width=\"552\" height=\"60\"></td></tr>"
                "<tr><td><h1 style=\"font-size:28px\">Autumn sale</h1><p style=\"font-size:16px\">Everything in the shop "
                "is 30% off until Sunday. <a href=\"https://example.org/shop\">Open the shop</a>.</p>"
                "<p style=\"font-size:8pt;color:#777777\">You get this letter because you subscribed. Unsubscribe.</p>"
                "</td></tr></table></td></tr></table>") : QStringLiteral(
                "<h2>October report</h2><p>Hello! The summary for <b>October</b> is below, the details are in "
                "<a href=\"https://example.org/report\">the full report</a>.</p>"
                "<table width=\"600\" align=\"center\" cellpadding=\"8\" style=\"border:1px solid #888\"><tr>"
                "<td style=\"font-size:8pt\">Payslip line, 8pt: salary 12 345,00 ₽, tax 1 605,00 ₽</td>"
                "<td width=\"40\"><img src=\"cid:logo@x\" width=\"40\" height=\"40\"></td></tr></table>"
                "<p style=\"font-size:14px\">A paragraph set in 14px, as newsletters do.</p>"
                "<blockquote>A short quote, two lines<br>of the earlier letter</blockquote>"));
            return QJsonObject{{"result", f}};
        });
        start();
        letterShown();
        const auto save = [this](const QString &path) {
            QTest::qWait(200);   // the attachments' and the letter's last paint
            QVERIFY(win->grabWindow().save(path));
        };
        save(shot + "-100.png");
        QSignalSpy docs(&s->letter, &Letter::documentChanged);
        s->letter.setZoom(1.3636);   // GNOME's text-scaling-factor on the author's machine
        QTRY_VERIFY_WITH_TIMEOUT(docs.size() >= 1, 10000);
        save(shot + "-136.png");
        *newsletter = true;
        win->resize(1060, 800);   // the letter's pane: ~600 px
        s->letter.reload();
        QTRY_VERIFY_WITH_TIMEOUT(s->letter.documentZoom() < 1.3, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(docs.size() >= 3, 10000);   // the resize's debounced layout too
        save(shot + "-fit.png");
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
    void rename_ends_when_the_keys_leave_it()
    {
        h.d->on("account_update", [](const QJsonObject &) { return QJsonObject{}; });
        start();
        s->app.pressAction(4);
        QQuickItem *rows = item("accountRows");
        QTRY_VERIFY(rows->hasActiveFocus());
        QTRY_COMPARE(rows->property("count").toInt(), 2);
        QTest::keyClick(win, Qt::Key_Return);   // the sender name of me@gmail.com
        QQuickItem *rename = itemAt(rows, 0)->findChild<QQuickItem *>("renameField");
        QCOMPARE(win->activeFocusItem(), rename);
        type(" X");

        QWindow other;   // another window and back: the edit stays, with the keys
        other.resize(100, 100);
        other.show();
        other.requestActivate();
        QTRY_VERIFY(!win->isActive());
        win->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(win));
        QCOMPARE(win->activeFocusItem(), rename);
        QVERIFY(rename->isVisible());

        QMetaObject::invokeMethod(item("emailField"), "forceActiveFocus");   // a click on the form: the edit ends
        QVERIFY(!rename->isVisible());
        QTest::keyClick(win, Qt::Key_Up);
        QTest::keyClick(win, Qt::Key_Up);
        QVERIFY(rows->hasActiveFocus());   // the list's keys, not the hidden field's
        QVERIFY(!rename->hasActiveFocus());
        QTest::keyClick(win, Qt::Key_Return);     // a new edit of the row, nothing saved
        QCOMPARE(win->activeFocusItem(), rename);
        QCOMPARE(rename->property("text").toString(), QString("Max"));
        QTest::keyClick(win, Qt::Key_Escape);
        QVERIFY(rows->hasActiveFocus());
        QVERIFY(!rename->hasActiveFocus());
        QTest::qWait(100);
        QVERIFY(h.sent("account_update").isEmpty());
    }
};

QTEST_MAIN(TstUi)
#include "tst_ui.moc"
