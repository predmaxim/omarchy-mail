#include "Nav.h"
#include <algorithm>

namespace om {

namespace {

int count(Row r, const NavShape &s)
{
    switch (r) {
    case Row::Actions: return s.actions;
    case Row::Pickers: return s.pickers;
    case Row::Outbox: return s.outbox;
    case Row::Attachments: return s.attachments;
    default: return 1;   // the search field, the list, and the body: always there
    }
}

// Nearest non-empty letter row in direction d from r, or r itself.
Row neighbour(Row r, int d, const NavShape &s)
{
    for (int i = int(r) + d; i >= int(Row::Outbox) && i <= int(Row::Body); i += d)
        if (count(Row(i), s) > 0)
            return Row(i);
    return r;
}

// A link's box starts on the screen: ↑/↓ step to it.
bool starts(const QRectF &r, const QRectF &v) { return !r.isEmpty() && r.top() >= v.top() && r.top() < v.bottom(); }
// Some of it is on the screen: it stays current.
bool shown(const QRectF &r, const QRectF &v) { return !r.isEmpty() && r.bottom() > v.top() && r.top() < v.bottom(); }

// The nearest link after `from` in direction `dir` that starts on the screen, else -1.
int next(const NavShape &s, int from, int dir)
{
    for (int i = from + dir; i >= 0 && i < int(s.links.size()); i += dir)
        if (starts(s.links.at(i), s.view))
            return i;
    return -1;
}

// The nearest link in direction `dir` on the same line as `from` (their boxes overlap vertically), else -1;
// a tie goes to document order.
int beside(const NavShape &s, int from, int dir)
{
    const QRectF f = s.links.at(from);
    int best = -1;
    qreal bestDx = 0;
    for (int i = 0; i < int(s.links.size()); ++i) {
        const QRectF r = s.links.at(i);
        const qreal dx = (r.center().x() - f.center().x()) * dir;
        if (r.isEmpty() || dx <= 0 || r.top() >= f.bottom() || r.bottom() <= f.top())
            continue;
        if (best < 0 || dx < bestDx) {
            best = i;
            bestDx = dx;
        }
    }
    return best;
}

Step cmd(Cmd c, int col = 0, int dir = 0) { return {c, col, dir, false, false}; }

}

void Nav::go(Row r, int col)
{
    if (r == m_row && col == m_col)
        return;
    m_row = r;
    m_col = col;
    Q_EMIT changed();
}

void Nav::reset(Row r, int col) { go(r, col); }

void Nav::enter(Row r, const NavShape &s) { go(r, r == Row::Body ? next(s, -1, 1) : 0); }

void Nav::scrolled(const NavShape &s)
{
    if (m_row == Row::Body && m_col >= 0 && (m_col >= s.links.size() || !shown(s.links.at(m_col), s.view)))
        go(Row::Body, -1);
}

Step Nav::press(const KeyPress &k, const NavShape &s)
{
    using A = KeyAction;
    const A a = k.action;
    const int dir = (a == A::Left || a == A::Up) ? -1 : 1;
    const int cap = std::max(0, count(m_row, s) - 1);
    if (m_row == Row::Body)
        scrolled(s);
    else if (m_col > cap)
        go(m_row, cap);   // the letter changed under the cursor: fewer attachments now

    switch (m_row) {
    case Row::Actions:
    case Row::Search:
    case Row::Pickers:
        if (a == A::Left || a == A::Right) {
            if (m_row != Row::Search)
                go(m_row, std::clamp(m_col + dir, 0, cap));
        } else if (a == A::Up || a == A::Down) {
            const Row n = Row(int(m_row) + dir);
            if (n >= Row::Actions && n <= Row::List)
                go(n, n == Row::Actions || n == Row::Pickers ? std::clamp(m_col, 0, std::max(0, count(n, s) - 1)) : 0);
        } else if (a == A::Enter) {
            if (m_row == Row::Actions) return cmd(Cmd::PressAction, m_col);
            if (m_row == Row::Pickers) return cmd(Cmd::OpenPicker, m_col);
            return cmd(Cmd::FocusSearch);
        }
        return {};
    case Row::List:
        if (a == A::Up && !k.ctrl && !k.shift && s.listAtTop) {
            go(Row::Pickers, std::clamp(m_col, 0, std::max(0, s.pickers - 1)));
            return {};
        }
        if (a == A::Up || a == A::Down)
            return {Cmd::ListMove, 0, dir, k.ctrl, k.shift};
        if (a == A::Home || a == A::End)
            return {Cmd::ListEdge, 0, a == A::Home ? -1 : 1, k.ctrl, k.shift};
        if (a == A::Right || a == A::Enter) {
            Row r = neighbour(Row::Outbox, 1, s);
            if (count(Row::Outbox, s) > 0)
                r = Row::Outbox;
            enter(r, s);
            return cmd(Cmd::EnterLetter);
        }
        if (a == A::Space || a == A::PageDown) return cmd(Cmd::ScrollPages, 0, 1);
        if (a == A::PageUp) return cmd(Cmd::ScrollPages, 0, -1);
        return {};
    default:
        break;
    }

    // letter rows
    if (a == A::Escape) { go(Row::List); return {}; }
    if (m_row == Row::Body) {
        switch (a) {
        case A::Up:
        case A::Down:
            if (const int i = next(s, (m_col < 0 && dir < 0) ? int(s.links.size()) : m_col, dir); i >= 0)
                go(Row::Body, i);
            else if (a == A::Down || s.view.top() > 0)
                return cmd(Cmd::ScrollLines, 0, dir);
            else if (const Row n = neighbour(Row::Body, -1, s); n != Row::Body)
                enter(n, s);   // the top of the text: the attachments or the unsent mail's buttons
            return {};
        case A::Left:
        case A::Right:
            if (const int i = m_col >= 0 ? beside(s, m_col, dir) : -1; i >= 0)
                go(Row::Body, i);
            else if (a == A::Left)
                go(Row::List);
            return {};
        case A::Space:
        case A::PageDown: return cmd(Cmd::ScrollPages, 0, 1);
        case A::PageUp: return cmd(Cmd::ScrollPages, 0, -1);
        case A::Home: return cmd(Cmd::ScrollTop);
        case A::End: return cmd(Cmd::ScrollEnd);
        case A::Enter: return m_col >= 0 ? cmd(Cmd::OpenLink, m_col) : Step{};
        default: return {};
        }
    }
    if (a == A::Left) {
        if (m_col == 0) go(Row::List);
        else go(m_row, m_col - 1);
        return {};
    }
    if (a == A::Right) {
        go(m_row, std::min(m_col + 1, cap));
        return {};
    }
    if (a == A::Up || a == A::Down) {
        if (const Row n = neighbour(m_row, dir, s); n != m_row)
            enter(n, s);
        return {};
    }
    switch (m_row) {
    case Row::Attachments:
        if (a == A::Enter) return cmd(Cmd::OpenAttachment, m_col);
        if (a == A::Space) return cmd(Cmd::PreviewAttachment, m_col);
        if (a == A::Save) return cmd(Cmd::SaveAttachment, m_col);
        break;
    case Row::Outbox:
        if (a == A::Enter) return cmd(Cmd::PressOutbox, m_col);
        break;
    default:
        break;
    }
    return {};
}

bool Confirm::press(const QString &what, qint64 nowMs)
{
    if (armed == what && nowMs - at <= 3000) {
        armed.clear();
        return true;
    }
    armed = what;
    at = nowMs;
    return false;
}

}
