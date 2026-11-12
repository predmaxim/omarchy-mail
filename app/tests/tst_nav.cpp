#include "Nav.h"
#include <QTest>

using namespace om;

static KeyPress kp(KeyAction a, bool ctrl = false, bool shift = false) { return {a, ctrl, shift}; }
// The letter on a 600 px screen scrolled to `y`; its links' boxes in view px.
static NavShape body(const QList<QRectF> &links, qreal y = 0)
{
    NavShape s;
    s.links = links;
    s.view = QRectF(0, y, 800, 600);
    return s;
}
static QRectF at(qreal x, qreal y) { return QRectF(x, y, 60, 20); }

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
        QCOMPARE(n.col(), 5);
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
    void list_home_end_go_to_the_edges()
    {
        Nav n;
        NavShape s;
        Step st = n.press(kp(KeyAction::Home), s);
        QCOMPARE(st.cmd, Cmd::ListEdge);
        QCOMPARE(st.dir, -1);
        st = n.press(kp(KeyAction::End, false, true), s);
        QCOMPARE(st.cmd, Cmd::ListEdge);
        QCOMPARE(st.dir, 1);
        QVERIFY(st.shift);
        QCOMPARE(n.row(), int(Row::List));
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
    {   // links at 100 and 1000; screen at 0, then at 650, then at 200
        Nav n; NavShape s = body({at(10, 100), at(10, 1000)});
        QCOMPARE(n.press(kp(KeyAction::Right), s).cmd, Cmd::EnterLetter);
        QCOMPARE(n.row(), int(Row::Body)); QCOMPARE(n.col(), 0);          // the first link on screen
        n.reset(); s.view.moveTop(650); n.press(kp(KeyAction::Enter), s); QCOMPARE(n.col(), 1);
        n.reset(); s.view.moveTop(200); n.press(kp(KeyAction::Right), s); QCOMPARE(n.col(), -1);   // only scrolling
        n.reset(); s.attachments = 2; n.press(kp(KeyAction::Right), s); QCOMPARE(n.row(), int(Row::Attachments));
        n.reset(); s.outbox = 2; n.press(kp(KeyAction::Enter), s); QCOMPARE(n.row(), int(Row::Outbox));
    }
    void esc_in_letter_returns_to_list()
    {
        Nav n;
        NavShape s;
        n.press(kp(KeyAction::Right), s);
        QCOMPARE(n.row(), int(Row::Body));
        n.press(kp(KeyAction::Escape), s);
        QCOMPARE(n.row(), int(Row::List));
    }
    void body_down_up_step_links_on_screen_else_scroll()
    {
        Nav n; NavShape s = body({at(10, 100), at(10, 300), at(10, 900)});
        n.press(kp(KeyAction::Right), s); QCOMPARE(n.col(), 0);
        n.press(kp(KeyAction::Down), s); QCOMPARE(n.col(), 1);
        Step st = n.press(kp(KeyAction::Down), s);                 // link 2 starts below the screen
        QCOMPARE(st.cmd, Cmd::ScrollLines); QCOMPARE(st.dir, 1); QCOMPARE(n.col(), 1);
        s.view.moveTop(400); n.scrolled(s); QCOMPARE(n.col(), -1);  // link 1 went off the top
        n.press(kp(KeyAction::Down), s); QCOMPARE(n.col(), 2);      // the first link on screen
        st = n.press(kp(KeyAction::Up), s);                         // link 1 is above the screen
        QCOMPARE(st.cmd, Cmd::ScrollLines); QCOMPARE(st.dir, -1); QCOMPARE(n.col(), 2);
    }
    void body_up_without_current_takes_last_on_screen()
    {
        Nav n; const NavShape s = body({at(10, 100), at(10, 300), at(10, 500)}, 50);
        n.reset(Row::Body, -1); n.press(kp(KeyAction::Up), s); QCOMPARE(n.col(), 2);
        n.reset(Row::Body, -1); n.press(kp(KeyAction::Down), s); QCOMPARE(n.col(), 0);
    }
    void body_up_at_top_goes_to_the_row_above()
    {
        Nav n; NavShape s = body({at(10, 100)}); s.attachments = 2;
        n.press(kp(KeyAction::Right), s); QCOMPARE(n.row(), int(Row::Attachments));
        n.press(kp(KeyAction::Down), s); QCOMPARE(n.row(), int(Row::Body)); QCOMPARE(n.col(), 0);
        n.press(kp(KeyAction::Up), s); QCOMPARE(n.row(), int(Row::Attachments));
        s.attachments = 0; s.outbox = 2; n.reset(Row::Body, 0);
        n.press(kp(KeyAction::Up), s); QCOMPARE(n.row(), int(Row::Outbox));
        s.outbox = 0; n.reset(Row::Body, 0);                        // nothing above: stays
        QCOMPARE(n.press(kp(KeyAction::Up), s).cmd, Cmd::None); QCOMPARE(n.row(), int(Row::Body));
        s.view.moveTop(50);                                         // not at the top: scrolls
        QCOMPARE(n.press(kp(KeyAction::Up), s).cmd, Cmd::ScrollLines);
    }
    void body_left_right_on_the_same_line()
    {   // 0, 1, 2 on one line (2 is an icon a little lower), 3 under them
        Nav n; const NavShape s = body({QRectF(10, 100, 60, 30), QRectF(100, 100, 60, 30), QRectF(300, 105, 40, 20), QRectF(100, 200, 60, 20)});
        n.reset(Row::Body, 0);
        n.press(kp(KeyAction::Right), s); QCOMPARE(n.col(), 1);
        n.press(kp(KeyAction::Right), s); QCOMPARE(n.col(), 2);
        QCOMPARE(n.press(kp(KeyAction::Right), s).cmd, Cmd::None); QCOMPARE(n.col(), 2);
        n.press(kp(KeyAction::Left), s); QCOMPARE(n.col(), 1);                   // the nearest, not the first
        n.reset(Row::Body, 3); n.press(kp(KeyAction::Right), s); QCOMPARE(n.col(), 3);
        n.press(kp(KeyAction::Left), s); QCOMPARE(n.row(), int(Row::List));      // none to the left: the list
        n.reset(Row::Body, -1); n.press(kp(KeyAction::Right), s); QCOMPARE(n.row(), int(Row::Body));
        n.press(kp(KeyAction::Left), s); QCOMPARE(n.row(), int(Row::List));
    }
    void body_scroll_keys_scroll_and_enter_opens()
    {
        Nav n; const NavShape s = body({at(10, 100), at(10, 300)});
        n.reset(Row::Body, 1);
        QCOMPARE(n.press(kp(KeyAction::PageDown), s).cmd, Cmd::ScrollPages);
        QCOMPARE(n.press(kp(KeyAction::PageUp), s).dir, -1);
        const Step sp = n.press(kp(KeyAction::Space), s); QCOMPARE(sp.cmd, Cmd::ScrollPages); QCOMPARE(sp.dir, 1);
        QCOMPARE(n.press(kp(KeyAction::Home), s).cmd, Cmd::ScrollTop);
        QCOMPARE(n.press(kp(KeyAction::End), s).cmd, Cmd::ScrollEnd);
        QCOMPARE(n.col(), 1);   // the screen decides what stays current: scrolled()
        const Step st = n.press(kp(KeyAction::Enter), s); QCOMPARE(st.cmd, Cmd::OpenLink); QCOMPARE(st.col, 1);
        QCOMPARE(n.press(kp(KeyAction::Save), s).cmd, Cmd::None);
        n.reset(Row::Body, -1); QCOMPARE(n.press(kp(KeyAction::Enter), s).cmd, Cmd::None);
    }
    void scrolled_drops_a_link_off_screen()
    {
        Nav n; NavShape s = body({at(10, 100), at(10, 300)});
        n.reset(Row::Body, 1);
        s.view.moveTop(310); n.scrolled(s); QCOMPARE(n.col(), 1);    // partly on screen: still current
        s.view.moveTop(320); n.scrolled(s); QCOMPARE(n.col(), -1);   // wholly above
        n.reset(Row::Body, 1); s.view.moveTop(0); s.links.clear(); n.scrolled(s); QCOMPARE(n.col(), -1);   // gone
        n.reset(Row::Attachments, 1); n.scrolled(s); QCOMPARE(n.col(), 1);   // other rows keep theirs
    }
    void links_without_a_box_are_skipped()
    {
        Nav n; const NavShape s = body({QRectF(), at(10, 100), QRectF()});
        n.press(kp(KeyAction::Right), s); QCOMPARE(n.col(), 1);
        QCOMPARE(n.press(kp(KeyAction::Down), s).cmd, Cmd::ScrollLines);
        n.reset(Row::Body, -1); n.press(kp(KeyAction::Up), s); QCOMPARE(n.col(), 1);
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
    void col_clamped_to_shape()
    {
        Nav n;
        NavShape s;
        s.attachments = 3;
        n.press(kp(KeyAction::Right), s);
        n.press(kp(KeyAction::Right), s);
        n.press(kp(KeyAction::Right), s);
        QCOMPARE(n.col(), 2);
        s.attachments = 1;   // another letter came under the cursor
        const Step st = n.press(kp(KeyAction::Enter), s);
        QCOMPARE(st.cmd, Cmd::OpenAttachment);
        QCOMPARE(st.col, 0);
        QCOMPARE(n.col(), 0);
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
