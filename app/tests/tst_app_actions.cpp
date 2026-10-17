#include "AppHarness.h"
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTimer>

using namespace om;
using namespace om::test;

class TstAppActions : public QObject, AppHarness {
    Q_OBJECT

    bool press(int key, quint32 scan = 0, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString &focus = {},
               bool repeat = false)
    {
        return app->key(key, scan, int(mods), repeat, focus);
    }
    QList<QJsonObject> acts(const QString &op) const
    {
        QList<QJsonObject> out;
        for (const QJsonObject &r : sent("act"))
            if (r.value("op").toString() == op)
                out << body(r);
        return out;
    }
    static QJsonObject act(QJsonArray ids, const QString &op) { return {{"cmd", "act"}, {"ids", ids}, {"op", op}}; }
    void unread(int row)
    {
        QJsonObject r = rows.at(row).toObject();
        r["seen"] = false;
        rows[row] = r;
    }
    void bootList()
    {
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
    }

private Q_SLOTS:
    void init()
    {
        setUp();
        d->on("act", [n = std::make_shared<int>(5)](const QJsonObject &) {
            return QJsonObject{{"result", QJsonObject{{"batch", (*n)++}}}};
        });
        d->on("undo", [](const QJsonObject &) { return QJsonObject{{"result", QJsonObject{{"batch", 100}}}}; });
    }
    void cleanup() { tearDown(); }

    void enter_on_the_quote_link_expands_it()
    {
        d->on("message", [](const QJsonObject &r) {
            QString lines;
            for (int i = 1; i <= 20; ++i)
                lines += QStringLiteral("Quoted line %1<br>").arg(i);
            return QJsonObject{{"result", QJsonObject{{"summary", QJsonObject{{"id", r.value("message")}}},
                                                      {"html", QStringLiteral("<p>See below.</p><blockquote>%1"
                                                                              "<a href=\"https://x.org/inside\">inside</a></blockquote>").arg(lines)}}}};
        });
        letter->setWidth(760);
        bootList();
        QTRY_COMPARE_WITH_TIMEOUT(letter->linkCount(), 1, 10000);   // "··· Show quoted text (1)"
        QVERIFY(press(Qt::Key_Right));
        QCOMPARE(nav->row(), int(Row::Links));
        QVERIFY(press(Qt::Key_Return, 36));
        QTRY_COMPARE_WITH_TIMEOUT(letter->links().value(0).url, QUrl("https://x.org/inside"), 10000);   // opened
        QCOMPARE(letter->linkCount(), 1);
        // W39: the links moved under the cursor: nothing stays picked that a second Enter would open
        QCOMPARE(nav->row(), int(Row::Text));
    }
    void d_deletes_and_moves_cursor()
    {
        bootList();
        QTRY_COMPARE(list->cursor(), 3);   // 57, the first unread
        QVERIFY(press(Qt::Key_D, 40));
        QCOMPARE(list->rowCount(), 49);
        QCOMPARE(list->cursorRow().value("id").toInteger(), 56);
        QTRY_COMPARE(acts("delete").size(), 1);
        QCOMPARE(acts("delete").at(0), act({57}, "delete"));
        QVERIFY(press(Qt::Key_Delete));
        QTRY_COMPARE(acts("delete").size(), 2);
        QCOMPARE(acts("delete").at(1), act({56}, "delete"));
    }

    void d_in_trash_needs_second_press()
    {
        bootList();
        app->pickFolder(3);   // Trash
        QVERIFY(app->trashView());
        barrier();            // Trash's rows are in
        const qint64 id = list->cursorRow().value("id").toInteger();
        QVERIFY(press(Qt::Key_D, 40));
        QVERIFY(app->note().contains("again"));
        barrier();
        QVERIFY(acts("delete").isEmpty());
        QVERIFY(acts("delete_forever").isEmpty());
        QCOMPARE(list->rowCount(), 50);
        QVERIFY(press(Qt::Key_D, 40));
        QTRY_COMPARE(acts("delete_forever").size(), 1);
        QCOMPARE(acts("delete_forever").at(0), act({id}, "delete_forever"));
        QCOMPARE(list->rowCount(), 49);
        barrier();   // the reply with its batch is in: deleting forever is not undoable
        QVERIFY(press(Qt::Key_Z, 52, Qt::ControlModifier));
        QCOMPARE(app->note(), QString("Nothing to undo"));
        barrier();
        QVERIFY(sent("undo").isEmpty());
    }

    void selection_actions()
    {
        unread(0);   // 60
        bootList();
        QCOMPARE(list->cursor(), 0);
        QVERIFY(press(Qt::Key_Space, 65, Qt::ControlModifier));
        QVERIFY(press(Qt::Key_Down, 0, Qt::ControlModifier));
        QVERIFY(press(Qt::Key_Down, 0, Qt::ControlModifier));
        QVERIFY(press(Qt::Key_Space, 65, Qt::ControlModifier));
        QCOMPARE(list->selectedCount(), 2);
        QCOMPARE(list->cursor(), 2);
        QVERIFY(press(Qt::Key_U, 30));   // one of them unread: both become read
        QTRY_COMPARE(acts("read").size(), 1);
        QCOMPARE(acts("read").at(0), act({60, 58}, "read"));
        QVERIFY(list->row(0).value("seen").toBool());
        QVERIFY(press(Qt::Key_F, 41));
        QTRY_COMPARE(acts("flag").size(), 1);
        QCOMPARE(acts("flag").at(0), act({60, 58}, "flag"));
        QVERIFY(list->row(2).value("flagged").toBool());
        QVERIFY(press(Qt::Key_F, 41));
        QTRY_COMPARE(acts("unflag").size(), 1);
        barrier();
        QVERIFY(press(Qt::Key_Z, 52, Qt::ControlModifier));   // the last action first: unflag (7)
        QVERIFY(press(Qt::Key_Z, 52, Qt::ControlModifier));
        QVERIFY(press(Qt::Key_Z, 52, Qt::ControlModifier));
        QTRY_COMPARE(sent("undo").size(), 3);
        QCOMPARE(body(sent("undo").at(0)), QJsonObject({{"cmd", "undo"}, {"batch", 7}}));
        QCOMPARE(body(sent("undo").at(2)), QJsonObject({{"cmd", "undo"}, {"batch", 5}}));   // undo's own batch isn't stacked
        QVERIFY(press(Qt::Key_Z, 52, Qt::ControlModifier));
        QCOMPARE(app->note(), QString("Nothing to undo"));
    }

    void auto_read_after_1s_not_undoable()
    {
        QElapsedTimer clock;
        clock.start();
        bootList();
        QTRY_COMPARE(letter->id(), 57);
        QTRY_VERIFY_WITH_TIMEOUT(!acts("read").isEmpty(), 3000);
        QVERIFY2(clock.elapsed() >= 950, qPrintable(QString::number(clock.elapsed())));   // coarse timers: 5 % early
        QCOMPARE(acts("read"), QList<QJsonObject>({act({57}, "read")}));
        QVERIFY(list->row(3).value("seen").toBool());
        QVERIFY(press(Qt::Key_D, 40));
        QTRY_COMPARE(acts("delete").size(), 1);
        barrier();
        QVERIFY(press(Qt::Key_Z, 52, Qt::ControlModifier));
        QTRY_COMPARE(sent("undo").size(), 1);
        QCOMPARE(body(sent("undo").at(0)), QJsonObject({{"cmd", "undo"}, {"batch", 6}}));   // the delete's, not the read's (5)
    }

    void auto_read_skipped_when_moved_away()
    {
        unread(4);   // 56, below 57
        bootList();
        QTRY_COMPARE(letter->id(), 57);
        QTest::qWait(500);
        QVERIFY(press(Qt::Key_Down));
        QCOMPARE(letter->id(), 56);
        QTRY_VERIFY_WITH_TIMEOUT(!acts("read").isEmpty(), 3000);   // 56's, a second after the move: 57's would be earlier
        QCOMPARE(acts("read"), QList<QJsonObject>({act({56}, "read")}));
        QVERIFY(!list->row(3).value("seen").toBool());
    }

    void unread_view_keeps_the_letter_being_read()
    {
        // as maild does: `act read` changes the rows, `changed` follows, the unread-only list drops read mail
        d->on("list", [this](const QJsonObject &r) {
            if (!r.value("unread_only").toBool())
                return QJsonObject{{"result", slice(r)}};
            QJsonArray out;
            for (const QJsonValue &v : std::as_const(rows))
                if (!v.toObject().value("seen").toBool())
                    out << v;
            return QJsonObject{{"result", out}};
        });
        d->on("act", [this](const QJsonObject &r) {
            const QJsonArray ids = r.value("ids").toArray();
            for (int i = 0; i < rows.size(); ++i)
                if (ids.contains(rows.at(i).toObject().value("id"))) {
                    QJsonObject o = rows.at(i).toObject();
                    o["seen"] = true;
                    rows[i] = o;
                }
            QTimer::singleShot(0, d, [this, ids] { d->emitEvent({{"event", "changed"}, {"folders", true}, {"ids", ids}}); });
            return QJsonObject{{"result", QJsonObject{{"batch", 5}}}};
        });
        bootList();
        QVERIFY(press(Qt::Key_U, 30, Qt::ShiftModifier));
        QTRY_COMPARE(list->rowCount(), 3);   // 57, 50, 40
        QCOMPARE(letter->id(), 57);
        const auto lists = sent("list").size();
        QTRY_VERIFY_WITH_TIMEOUT(!acts("read").isEmpty(), 3000);
        QTRY_COMPARE(sent("list").size(), lists + 1);   // `changed` reloads the list: 57 is not in it
        barrier();
        QCOMPARE(list->rowCount(), 3);
        QCOMPARE(list->cursor(), 0);
        QCOMPARE(list->cursorRow().value("id").toInteger(), 57);
        QVERIFY(list->cursorRow().value("seen").toBool());
        QCOMPARE(letter->id(), 57);
        QTest::qWait(1300);   // a chain would read 50 a second after the reload
        barrier();
        QCOMPARE(acts("read"), QList<QJsonObject>({act({57}, "read")}));

        QVERIFY(press(Qt::Key_Down));   // the user moves on: 50 is read, 57 goes with that reload
        QCOMPARE(letter->id(), 50);
        QTRY_COMPARE_WITH_TIMEOUT(acts("read").size(), 2, 3000);
        QCOMPARE(acts("read").at(1), act({50}, "read"));
        QTRY_COMPARE(list->rowCount(), 2);
        QCOMPARE(list->row(0).value("id").toInteger(), 50);
        QCOMPARE(list->row(1).value("id").toInteger(), 40);
        QCOMPARE(list->cursorRow().value("id").toInteger(), 50);
        QCOMPARE(letter->id(), 50);
    }

    void search_results_are_not_auto_read()
    {
        bootList();
        d->on("search", [this](const QJsonObject &) {
            return QJsonObject{{"result", QJsonObject{{"local", QJsonArray{rows.at(20)}}, {"token", 7}}}};   // 40, unread
        });
        app->setQuery("x");
        QTRY_COMPARE(letter->id(), 40);
        QTest::qWait(1300);
        barrier();
        QVERIFY(!acts("read").contains(act({40}, "read")));
        QVERIFY(press(Qt::Key_Return, 36, Qt::NoModifier, "search"));   // the user goes to the results
        QTRY_VERIFY_WITH_TIMEOUT(acts("read").contains(act({40}, "read")), 3000);
    }

    void outbox_row_discard_and_retry()
    {
        QJsonObject out = rows.at(0).toObject();
        out["id"] = -3;
        out["role"] = "sent";
        out["queued"] = true;
        out["failed"] = true;
        const QJsonArray sentRows{out, rows.at(1), rows.at(2)};
        d->on("list", [this, sentRows](const QJsonObject &r) {
            return QJsonObject{{"result", r.value("role").toString() == "sent" ? sentRows : slice(r)}};
        });
        bootList();
        app->pickFolder(2);   // Sent
        QTRY_COMPARE(list->cursorRow().value("id").toInteger(), -3);
        QVERIFY(press(Qt::Key_D, 40));
        QCOMPARE(app->note(), QString("Press d again to discard the unsent message"));
        barrier();
        QVERIFY(sent("outbox_discard").isEmpty());
        QVERIFY(press(Qt::Key_D, 40));
        QTRY_COMPARE(sent("outbox_discard").size(), 1);
        QCOMPARE(body(sent("outbox_discard").at(0)), QJsonObject({{"cmd", "outbox_discard"}, {"outbox", -3}}));
        QVERIFY(sent("act").isEmpty());
        QCOMPARE(list->rowCount(), 3);   // maild's `changed` takes the row away

        QTRY_COMPARE(letter->id(), -3);
        app->retryOutbox();
        QTRY_COMPARE(sent("outbox_retry").size(), 1);
        QCOMPARE(body(sent("outbox_retry").at(0)), QJsonObject({{"cmd", "outbox_retry"}, {"outbox", -3}}));
        app->discardOutbox();   // the letter pane's button: the same second press
        barrier();
        QCOMPARE(sent("outbox_discard").size(), 1);
        app->discardOutbox();
        QTRY_COMPARE(sent("outbox_discard").size(), 2);
    }

    void remote_rows_skip_actions()
    {
        bootList();
        d->on("search", [](const QJsonObject &) {
            return QJsonObject{{"result", QJsonObject{{"local", QJsonArray()}, {"token", 7}}}};
        });
        app->setQuery("ozon");
        QTRY_COMPARE(list->rowCount(), 0);
        d->emitEvent({{"event", "search_remote"}, {"token", 7}, {"hits", QJsonArray{hit("Ozon receipt", 2)}}});
        QTRY_COMPARE(list->rowCount(), 1);
        QCOMPARE(list->cursor(), 0);
        QVERIFY(press(Qt::Key_D, 40));
        QCOMPARE(app->note(), QString("Server search results can't be changed"));
        QVERIFY(press(Qt::Key_U, 30));
        barrier();
        QVERIFY(sent("act").isEmpty());
        QCOMPARE(list->rowCount(), 1);
    }

    void ctrl_a_selects_all_loaded()
    {
        bootList();
        QVERIFY(press(Qt::Key_A, 38, Qt::ControlModifier));
        QCOMPARE(list->selectedCount(), 50);
        QCOMPARE(list->selectedCount(), list->rowCount());
        QVERIFY(press(Qt::Key_Right));
        app->clickRow(6, int(Qt::ControlModifier));
        QCOMPARE(list->selectedCount(), 49);
        QCOMPARE(list->cursor(), 6);
        QCOMPARE(nav->row(), int(Row::List));
        app->clickRow(4, int(Qt::NoModifier));
        QCOMPARE(list->selectedCount(), 0);
        QCOMPARE(list->cursor(), 4);
    }

    void action_bar_buttons()
    {
        bootList();
        app->pressAction(3);
        QVERIFY(app->unreadOnly());
        app->pressAction(0);
        QVERIFY(compose->isOpen());
        app->pressAction(4);
        QCOMPARE(app->page(), QString("settings"));
    }

    void action_bar_under_the_form()   // the list behind the form: its buttons do nothing
    {
        bootList();
        QTRY_VERIFY(!letter->full().isEmpty());
        app->pressAction(0);
        QVERIFY(compose->isOpen());
        compose->setSubject("Draft");
        for (int col : {0, 1, 2, 3})
            app->pressAction(col);
        QVERIFY(compose->isOpen() && !compose->isReply());
        QCOMPARE(compose->subject(), QString("Draft"));
        QVERIFY(!app->unreadOnly());
        QCOMPARE(list->rowCount(), 50);
        barrier();
        QVERIFY(acts("delete").isEmpty());
        app->pressAction(4);   // the gear: the form closes with its draft kept, the mailboxes page opens
        QVERIFY(!compose->isOpen());
        QCOMPARE(app->page(), QString("settings"));
        QVERIFY(QFile::exists(dir->filePath("drafts/new.json")));
        app->closeSettings();
        app->composeNew();
        QCOMPARE(compose->subject(), QString("Draft"));
    }

    void notification_click_closes_the_form()
    {
        bootList();
        compose->startNew("me@gmail.com");
        compose->setSubject("Half");
        d->emitEvent({{"event", "open"}, {"id", 59}});
        QTRY_COMPARE(letter->id(), 59);
        QTRY_COMPARE(list->cursor(), 1);
        QVERIFY(!compose->isOpen());   // the letter is in sight, not under the form
        QVERIFY(QFile::exists(dir->filePath("drafts/new.json")));
    }

    void letter_under_the_form_is_not_auto_read()
    {
        bootList();
        QTRY_COMPARE(letter->id(), 57);
        QVERIFY(press(Qt::Key_C, 54));   // the form covers the letter before its second is up
        QTest::qWait(1300);
        barrier();
        QVERIFY(acts("read").isEmpty());
    }

    void seen_letter_is_not_read_again()
    {
        bootList();
        QTRY_COMPARE(list->cursor(), 3);
        QVERIFY(press(Qt::Key_Down));   // 56: read already
        QCOMPARE(letter->id(), 56);
        QTest::qWait(1300);
        barrier();
        QVERIFY(acts("read").isEmpty());
    }

    void keys_act_on_the_letter_shown()   // W16: `--open` a letter that isn't among the rows
    {
        boot(7);
        QTRY_COMPARE(list->rowCount(), 50);
        QTRY_COMPARE(letter->id(), 7);
        QCOMPARE(list->cursorRow().value("id").toInteger(), 57);
        QVERIFY(press(Qt::Key_U, 30));
        QTRY_COMPARE(acts("read"), QList<QJsonObject>({act({7}, "read")}));
        QVERIFY(press(Qt::Key_F, 41));
        QTRY_COMPARE(acts("flag"), QList<QJsonObject>({act({7}, "flag")}));
        QVERIFY(press(Qt::Key_D, 40));
        QTRY_COMPARE(acts("delete"), QList<QJsonObject>({act({7}, "delete")}));
        QCOMPARE(list->rowCount(), 50);   // the cursor row untouched
        QCOMPARE(list->cursorRow().value("id").toInteger(), 57);
        QVERIFY(!list->cursorRow().value("seen").toBool());
        QCOMPARE(letter->id(), 57);   // the letter deleted, the pane follows the cursor again
    }

    void esc_chain()
    {
        bootList();
        d->on("search", [this](const QJsonObject &) {
            return QJsonObject{{"result", QJsonObject{{"local", QJsonArray{rows.at(0), rows.at(5)}}, {"token", 7}}}};
        });
        QSignalSpy closed(app, &App::closeRequested);
        app->setQuery("ozon");
        QTRY_COMPARE(list->rowCount(), 2);
        QVERIFY(press(Qt::Key_A, 38, Qt::ControlModifier));
        QVERIFY(press(Qt::Key_Right));
        QVERIFY(nav->row() != int(Row::List));
        const auto lists = sent("list").size();

        QVERIFY(press(Qt::Key_Escape));   // the letter's rows: back to the list
        QCOMPARE(nav->row(), int(Row::List));
        QCOMPARE(list->selectedCount(), 2);
        QVERIFY(press(Qt::Key_Escape));   // the selection
        QCOMPARE(list->selectedCount(), 0);
        QVERIFY(app->searching());
        QVERIFY(press(Qt::Key_Escape));   // the search
        QVERIFY(!app->searching());
        QCOMPARE(app->query(), QString());
        QTRY_COMPARE(sent("list").size(), lists + 1);
        QCOMPARE(body(sent("list").last()), inboxList(0, 50));
        QCOMPARE(closed.size(), 0);
        QVERIFY(press(Qt::Key_Escape));
        QCOMPARE(closed.size(), 1);
    }

    void esc_closes_settings_first()
    {
        bootList();
        QSignalSpy closed(app, &App::closeRequested);
        app->openSettings();
        QVERIFY(!press(Qt::Key_D, 40));   // the mailbox page's own keys
        QVERIFY(press(Qt::Key_Escape));
        QCOMPARE(app->page(), QString("main"));
        QCOMPARE(closed.size(), 0);
    }

    void unread_filter_shift_u()
    {
        bootList();
        QVERIFY(press(Qt::Key_U, 30, Qt::ShiftModifier));
        QVERIFY(app->unreadOnly());
        QTRY_COMPARE(body(sent("list").last()),
                     QJsonObject({{"cmd", "list"}, {"role", "inbox"}, {"unread_only", true}, {"offset", 0}, {"limit", 50}}));
        barrier();
        QVERIFY(sent("act").isEmpty());   // not `u`
    }

    void keys_russian_layout()
    {
        bootList();
        QTRY_COMPARE(list->cursor(), 3);
        QVERIFY(press(0x0412, 40));   // В
        QTRY_COMPARE(acts("delete"), QList<QJsonObject>({act({57}, "delete")}));
        barrier();
        QVERIFY(press(0x042F, 52, Qt::ControlModifier));   // Я
        QTRY_COMPARE(sent("undo").size(), 1);
        QCOMPARE(body(sent("undo").at(0)), QJsonObject({{"cmd", "undo"}, {"batch", 5}}));
    }

    void held_keys_only_move()
    {
        bootList();
        QTRY_COMPARE(list->cursor(), 3);
        QVERIFY(press(Qt::Key_Down, 0, Qt::NoModifier, {}, true));
        QCOMPARE(list->cursor(), 4);
        QVERIFY(press(Qt::Key_D, 40, Qt::NoModifier, {}, true));   // eaten, not acted on
        barrier();
        QVERIFY(sent("act").isEmpty());
        QCOMPARE(list->rowCount(), 50);
    }

    void nav_commands()
    {
        bootList();
        QSignalSpy scrolled(app, &App::scroll);
        QVERIFY(press(Qt::Key_Space));   // on a list row: pages through the letter
        QCOMPARE(scrolled.size(), 1);
        QCOMPARE(scrolled.at(0).at(0).toInt(), int(Cmd::ScrollPages));
        QCOMPARE(scrolled.at(0).at(1).toInt(), 1);
        QVERIFY(!press(Qt::Key_C, 54, Qt::ControlModifier));   // the letter's selected text: QML copies it

        QSignalSpy picker(app, &App::openPicker);
        QSignalSpy search(app, &App::focusSearch);
        list->setCursor(0);
        QVERIFY(press(Qt::Key_Up));   // above the first row: the pickers
        QCOMPARE(nav->row(), int(Row::Pickers));
        QVERIFY(press(Qt::Key_Return, 36));
        QVERIFY(press(Qt::Key_Right));
        QVERIFY(press(Qt::Key_Return, 36));
        QCOMPARE(picker.size(), 2);
        QCOMPARE(picker.at(0).at(0).toString(), QString("account"));
        QCOMPARE(picker.at(1).at(0).toString(), QString("folder"));
        QVERIFY(press(Qt::Key_Up));
        QVERIFY(press(Qt::Key_Return, 36));
        QCOMPARE(search.size(), 1);
        QVERIFY(press(Qt::Key_Escape));
        QCOMPARE(nav->row(), int(Row::List));
        QVERIFY(press(Qt::Key_Period, 61));   // `/` in the Russian layout
        QCOMPARE(search.size(), 2);
        QCOMPARE(nav->row(), int(Row::Search));
    }

    void compose_keys()
    {
        d->on("send", [](const QJsonObject &) { return QJsonObject{{"result", QJsonObject{{"outbox", 3}}}}; });
        bootList();
        QVERIFY(press(Qt::Key_C, 54));
        QVERIFY(compose->isOpen());
        QCOMPARE(compose->account(), QString("me@gmail.com"));   // all mailboxes viewed: the first one
        QVERIFY(!press(Qt::Key_D, 40, Qt::NoModifier, "compose"));   // typed into the form, no mail deleted
        QVERIFY(!press(Qt::Key_D, 40));   // nor with the window's focus: the list is disabled under the form
        QVERIFY(!press(Qt::Key_V, 55, Qt::ControlModifier, "compose"));   // no image or files: the field pastes text
        QSignalSpy pick(app, &App::pickFiles);
        QVERIFY(press(Qt::Key_O, 32, Qt::ControlModifier, "compose"));
        QCOMPARE(pick.size(), 1);
        QVERIFY(press(Qt::Key_Return, 36, Qt::ControlModifier, "compose"));   // no recipient yet
        QCOMPARE(compose->note(), QString("Add a recipient"));
        QVERIFY(compose->noteError());
        compose->setTo("ivan@example.org");
        compose->setSubject("Hi");
        compose->setBody("x");
        QVERIFY(press(Qt::Key_Return, 36, Qt::ControlModifier, "compose"));
        QTRY_VERIFY(!compose->isOpen());
        QCOMPARE(sent("send").size(), 1);
        const QJsonObject s = body(sent("send").at(0));
        for (const char *k : {"cmd", "account", "to", "subject", "text"})
            QVERIFY2(s.contains(k), k);
        QCOMPARE(s.value("to").toArray(), QJsonArray({QJsonObject{{"name", ""}, {"addr", "ivan@example.org"}}}));
        QCOMPARE(app->note(), QString("Sending…"));
        barrier();
        QVERIFY(sent("act").isEmpty());

        QVERIFY(press(Qt::Key_C, 54));
        QVERIFY(press(Qt::Key_Escape, 0, Qt::NoModifier, "compose"));
        QVERIFY(!compose->isOpen());
    }

    void send_finishing_after_draft_switch()
    {
        d->on("send", [](const QJsonObject &) { return QJsonObject{{"result", QJsonObject{{"outbox", 3}}}, {"delay", 300}}; });
        bootList();
        QTRY_VERIFY(!letter->full().isEmpty());
        const QString newDraft = dir->filePath("drafts/new.json"), replyDraft = dir->filePath("drafts/reply-57.json");
        compose->startNew("me@gmail.com");
        compose->setSubject("Later");
        compose->close();
        QVERIFY(QFile::exists(newDraft));

        QVERIFY(press(Qt::Key_R, 27));
        compose->setTo("ivan@example.org");
        QGuiApplication::clipboard()->setImage(QImage(4, 4, QImage::Format_RGB32));
        QVERIFY(press(Qt::Key_V, 55, Qt::ControlModifier, "compose"));
        const QString shot = dir->filePath("drafts/files/") + compose->attachments().value(0).toMap().value("name").toString();
        QVERIFY(QFile::exists(shot));
        QVERIFY(press(Qt::Key_Return, 36, Qt::ControlModifier, "compose"));
        QVERIFY(press(Qt::Key_Escape, 9, Qt::NoModifier, "compose"));   // while maild still reads the files
        QVERIFY(QFile::exists(replyDraft));
        QVERIFY(press(Qt::Key_C, 54));   // the saved new draft
        QCOMPARE(compose->subject(), QString("Later"));

        QTRY_COMPARE(app->note(), QString("Sending…"));
        QVERIFY(compose->isOpen());
        QCOMPARE(compose->subject(), QString("Later"));
        QVERIFY(QFile::exists(newDraft));
        QVERIFY(!QFile::exists(replyDraft));   // the sent one goes, with its screenshot
        QVERIFY(!QFile::exists(shot));
    }

    void send_error_after_esc_goes_to_window_note()
    {
        d->on("send", [](const QJsonObject &) { return QJsonObject{{"error", "cannot read /home/me/a.pdf"}, {"delay", 200}}; });
        bootList();
        compose->startNew("me@gmail.com");
        compose->setTo("ivan@example.org");
        QVERIFY(press(Qt::Key_Return, 36, Qt::ControlModifier, "compose"));
        QVERIFY(press(Qt::Key_Escape, 9, Qt::NoModifier, "compose"));
        QTRY_COMPARE(app->note(), QString("Not sent: cannot read /home/me/a.pdf"));
        QVERIFY(app->noteError());
        QVERIFY(QFile::exists(dir->filePath("drafts/new.json")));   // kept for another try

        QVERIFY(press(Qt::Key_C, 54));   // the form still open: the error is the form's
        QVERIFY(press(Qt::Key_Return, 36, Qt::ControlModifier, "compose"));
        QTRY_COMPARE(compose->note(), QString("cannot read /home/me/a.pdf"));
        QVERIFY(compose->noteError());
    }

    void reply_and_mailto()
    {
        bootList();
        QTRY_VERIFY(!letter->full().isEmpty());
        QVERIFY(press(Qt::Key_R, 27));
        QVERIFY(compose->isOpen());
        QVERIFY(compose->isReply());
        compose->close();
        Q_EMIT letter->composeTo(QUrl("mailto:anna@example.org?subject=Hi"));
        QVERIFY(compose->isOpen());
        QVERIFY(!compose->isReply());
        QCOMPARE(compose->to(), QString("anna@example.org"));
        QCOMPARE(compose->account(), QString("me@gmail.com"));
    }

    void search_focus_keys()
    {
        bootList();
        QSignalSpy left(app, &App::leaveSearch);
        app->setQuery("x");
        const auto lists = sent("list").size();
        QVERIFY(press(Qt::Key_Escape, 9, Qt::NoModifier, "search"));
        QCOMPARE(app->query(), QString());
        QTRY_COMPARE(sent("list").size(), lists + 1);
        QCOMPARE(body(sent("list").last()), inboxList(0, 50));
        QCOMPARE(left.size(), 0);   // the field keeps the keys
        QVERIFY(press(Qt::Key_Escape, 9, Qt::NoModifier, "search"));   // empty: leave the field
        QCOMPARE(left.size(), 1);
        QVERIFY(!press(Qt::Key_D, 40, Qt::NoModifier, "search"));   // typed into the field
        list->setCursor(5);
        QVERIFY(press(Qt::Key_Down, 0, Qt::NoModifier, "search"));
        QCOMPARE(list->cursor(), 0);
        QCOMPARE(nav->row(), int(Row::List));
        QCOMPARE(left.size(), 2);
    }

    void letter_zoom_keys()
    {
        QTemporaryDir config(scratch());   // not the shared test home
        const QByteArray old = qgetenv("XDG_CONFIG_HOME");
        const auto restore = qScopeGuard([old] { qputenv("XDG_CONFIG_HOME", old); });
        qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
        const QString file = config.filePath("omarchy-mail/window.json");
        const auto saved = [file] {
            QFile f(file);
            return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
        };
        const Qt::KeyboardModifiers ctrl = Qt::ControlModifier;
        bootList();
        QCOMPARE(letter->zoom(), 1.0);   // the harness reads no GNOME factor
        QVERIFY(press(Qt::Key_Equal, 21, ctrl));
        QCOMPARE(letter->zoom(), 1.1);
        QCOMPARE(app->note(), QString("Letter zoom 110%"));
        QCOMPARE(saved().value("letterZoom").toDouble(), 1.1);
        QCOMPARE(QFileInfo(file).permissions() & 0x7777,
                 QFile::Permissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser));
        QCOMPARE(QFileInfo(config.filePath("omarchy-mail")).permissions() & 0x0077, QFile::Permissions());
        QVERIFY(press(Qt::Key_Minus, 20, ctrl, "search"));   // from the search field too
        QCOMPARE(letter->zoom(), 1.0);
        QVERIFY(press(Qt::Key_Minus, 82, ctrl | Qt::KeypadModifier));
        QCOMPARE(app->note(), QString("Letter zoom 91%"));
        for (int i = 0; i < 30; ++i)
            press(Qt::Key_Plus, 21, ctrl | Qt::ShiftModifier, {}, i > 0);   // held: it keeps zooming
        QCOMPARE(letter->zoom(), 3.0);
        QCOMPARE(app->note(), QString("Letter zoom 300%"));
        QVERIFY(press(Qt::Key_0, 19, ctrl));
        QCOMPARE(letter->zoom(), 1.0);   // back to the default, which follows GNOME again
        QVERIFY(QFileInfo::exists(file));
        QVERIFY(!saved().contains("letterZoom"));
        QCOMPARE(app->note(), QString("Letter zoom 100%"));

        QVERIFY(press(Qt::Key_C, 54));   // the form: no letter on the screen
        QVERIFY(compose->isOpen());
        QVERIFY(!press(Qt::Key_Equal, 21, ctrl, "compose"));
        QVERIFY(!press(Qt::Key_Equal, 21, ctrl));
        QVERIFY(press(Qt::Key_Escape, 9, Qt::NoModifier, "compose"));
        app->openSettings();   // the mailboxes page
        QVERIFY(!press(Qt::Key_Equal, 21, ctrl));
        QCOMPARE(letter->zoom(), 1.0);
    }

    void act_error_brings_rows_back()
    {
        d->on("act", [](const QJsonObject &) { return QJsonObject{{"error", "me@yandex.ru has no Trash folder"}}; });
        bootList();
        QTRY_COMPARE(list->cursor(), 3);
        const auto lists = sent("list").size();
        QVERIFY(press(Qt::Key_D, 40));
        QCOMPARE(list->rowCount(), 49);
        QTRY_COMPARE(app->note(), QString("me@yandex.ru has no Trash folder"));
        QVERIFY(app->noteError());
        QTRY_COMPARE(list->rowCount(), 50);   // reloaded from maild
        QCOMPARE(sent("list").size(), lists + 1);
        QCOMPARE(list->row(3).value("id").toInteger(), 57);
        QVERIFY(press(Qt::Key_Z, 52, Qt::ControlModifier));
        QCOMPARE(app->note(), QString("Nothing to undo"));
    }
};

QTEST_MAIN(TstAppActions)
#include "tst_app_actions.moc"
