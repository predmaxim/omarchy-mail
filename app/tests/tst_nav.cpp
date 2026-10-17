#include "Nav.h"
#include <QTest>

using namespace om;

static KeyPress kp(KeyAction a, bool ctrl = false, bool shift = false) { return {a, ctrl, shift}; }

class TstNav : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void walk_top_rows()
    {
        Nav n;
        NavShape s;
        s.listAtTop = true;
        n.press(kp(KeyAction::Up), s);
        QCOMPARE(n.row(), int(Row::Pickers));
        n.press(kp(KeyAction::Up), s);
        QCOMPARE(n.row(), int(Row::Search));
        n.press(kp(KeyAction::Up), s);
        QCOMPARE(n.row(), int(Row::Actions));
        n.press(kp(KeyAction::Down), s);
        n.press(kp(KeyAction::Down), s);
        n.press(kp(KeyAction::Down), s);
        QCOMPARE(n.row(), int(Row::List));
    }
    void top_row_enter_and_cols()
    {
        Nav n;
        NavShape s;
        n.reset(Row::Actions);
        n.press(kp(KeyAction::Right), s);
        n.press(kp(KeyAction::Right), s);
        QCOMPARE(n.col(), 2);
        Step st = n.press(kp(KeyAction::Enter), s);
        QCOMPARE(st.cmd, Cmd::PressAction);
        QCOMPARE(st.col, 2);
        for (int i = 0; i < 9; ++i)
            n.press(kp(KeyAction::Right), s);
        QCOMPARE(n.col(), 4);
        n.press(kp(KeyAction::Down), s);
        QCOMPARE(n.press(kp(KeyAction::Enter), s).cmd, Cmd::FocusSearch);
        n.press(kp(KeyAction::Down), s);
        n.press(kp(KeyAction::Right), s);
        st = n.press(kp(KeyAction::Enter), s);
        QCOMPARE(st.cmd, Cmd::OpenPicker);
        QCOMPARE(st.col, 1);
    }
    void list_up_not_at_top_moves_list()
    {
        Nav n;
        NavShape s;
        Step st = n.press(kp(KeyAction::Up, true, false), s);
        QCOMPARE(st.cmd, Cmd::ListMove);
        QCOMPARE(st.dir, -1);
        QVERIFY(st.ctrl);
        QCOMPARE(n.row(), int(Row::List));
        s.listAtTop = true;
        st = n.press(kp(KeyAction::Up, false, true), s);
        QCOMPARE(st.cmd, Cmd::ListMove);
        QCOMPARE(n.row(), int(Row::List));
        st = n.press(kp(KeyAction::Down), s);
        QCOMPARE(st.dir, 1);
    }
    void pickers_single_list()
    {
        Nav n;
        NavShape s;
        s.pickers = 1;
        n.reset(Row::Pickers);
        n.press(kp(KeyAction::Right), s);
        QCOMPARE(n.col(), 0);
    }
    void right_enters_first_available()
    {
        Nav n;
        NavShape s;
        s.links = 2;
        Step st = n.press(kp(KeyAction::Right), s);
        QCOMPARE(st.cmd, Cmd::EnterLetter);
        QCOMPARE(n.row(), int(Row::Links));
        QCOMPARE(n.col(), 0);
        n.reset();
        s.outbox = 2;
        n.press(kp(KeyAction::Enter), s);
        QCOMPARE(n.row(), int(Row::Outbox));
        n.reset();
        s.outbox = 0;
        s.links = 0;
        n.press(kp(KeyAction::Right), s);
        QCOMPARE(n.row(), int(Row::Text));
    }
    void left_on_first_returns_to_list()
    {
        Nav n;
        NavShape s;
        s.links = 2;
        n.press(kp(KeyAction::Right), s);
        n.press(kp(KeyAction::Right), s);
        QCOMPARE(n.col(), 1);
        n.press(kp(KeyAction::Left), s);
        QCOMPARE(n.col(), 0);
        QCOMPARE(n.row(), int(Row::Links));
        n.press(kp(KeyAction::Left), s);
        QCOMPARE(n.row(), int(Row::List));
    }
    void esc_in_letter_returns_to_list()
    {
        Nav n;
        NavShape s;
        n.press(kp(KeyAction::Right), s);
        QCOMPARE(n.row(), int(Row::Text));
        n.press(kp(KeyAction::Escape), s);
        QCOMPARE(n.row(), int(Row::List));
    }
    void text_up_at_top_goes_up()
    {
        Nav n;
        NavShape s;
        s.links = 1;
        n.press(kp(KeyAction::Right), s);
        n.press(kp(KeyAction::Down), s);
        QCOMPARE(n.row(), int(Row::Text));
        s.textAtTop = false;
        Step st = n.press(kp(KeyAction::Up), s);
        QCOMPARE(st.cmd, Cmd::ScrollLines);
        QCOMPARE(st.dir, -1);
        QCOMPARE(n.row(), int(Row::Text));
        st = n.press(kp(KeyAction::Down), s);
        QCOMPARE(st.dir, 1);
        s.textAtTop = true;
        n.press(kp(KeyAction::Up), s);
        QCOMPARE(n.row(), int(Row::Links));
        QCOMPARE(n.press(kp(KeyAction::PageDown), s).cmd, Cmd::None);
        n.press(kp(KeyAction::Down), s);
        QCOMPARE(n.press(kp(KeyAction::PageUp), s).dir, -1);
        QCOMPARE(n.press(kp(KeyAction::Home), s).cmd, Cmd::ScrollTop);
        QCOMPARE(n.press(kp(KeyAction::End), s).cmd, Cmd::ScrollEnd);
    }
    void attachment_commands()
    {
        Nav n;
        NavShape s;
        s.attachments = 3;
        n.press(kp(KeyAction::Right), s);
        n.press(kp(KeyAction::Right), s);
        QCOMPARE(n.col(), 1);
        Step st = n.press(kp(KeyAction::Enter), s);
        QCOMPARE(st.cmd, Cmd::OpenAttachment);
        QCOMPARE(st.col, 1);
        st = n.press(kp(KeyAction::Space), s);
        QCOMPARE(st.cmd, Cmd::PreviewAttachment);
        QCOMPARE(st.col, 1);
        st = n.press(kp(KeyAction::Save), s);
        QCOMPARE(st.cmd, Cmd::SaveAttachment);
        QCOMPARE(st.col, 1);
    }
    void link_enter_opens()
    {
        Nav n;
        NavShape s;
        s.links = 2;
        n.press(kp(KeyAction::Right), s);
        n.press(kp(KeyAction::Right), s);
        Step st = n.press(kp(KeyAction::Enter), s);
        QCOMPARE(st.cmd, Cmd::OpenLink);
        QCOMPARE(st.col, 1);
    }
    void outbox_enter()
    {
        Nav n;
        NavShape s;
        s.outbox = 2;
        n.press(kp(KeyAction::Right), s);
        n.press(kp(KeyAction::Right), s);
        Step st = n.press(kp(KeyAction::Enter), s);
        QCOMPARE(st.cmd, Cmd::PressOutbox);
        QCOMPARE(st.col, 1);
    }
    void space_on_list_scrolls_letter()
    {
        Nav n;
        NavShape s;
        Step st = n.press(kp(KeyAction::Space), s);
        QCOMPARE(st.cmd, Cmd::ScrollPages);
        QCOMPARE(st.dir, 1);
        QCOMPARE(n.row(), int(Row::List));
        QCOMPARE(n.press(kp(KeyAction::PageUp), s).dir, -1);
    }
    void confirm_gate()
    {
        Confirm c;
        QVERIFY(!c.press("d", 0));
        QVERIFY(c.press("d", 1000));
        Confirm late;
        QVERIFY(!late.press("d", 0));
        QVERIFY(!late.press("d", 3500));
        Confirm other;
        QVERIFY(!other.press("d", 0));
        QVERIFY(!other.press("x", 100));
    }
};

QTEST_MAIN(TstNav)
#include "tst_nav.moc"
