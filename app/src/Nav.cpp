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
    case Row::Links: return s.links;
    case Row::Text: return 1;
    default: return 1;
    }
}

// Nearest non-empty letter row in direction d from r, or r itself.
Row neighbour(Row r, int d, const NavShape &s)
{
    for (int i = int(r) + d; i >= int(Row::Outbox) && i <= int(Row::Text); i += d)
        if (count(Row(i), s) > 0)
            return Row(i);
    return r;
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

void Nav::reset(Row r) { go(r); }

Step Nav::press(const KeyPress &k, const NavShape &s)
{
    using A = KeyAction;
    const A a = k.action;
    const int dir = (a == A::Left || a == A::Up) ? -1 : 1;
    const int cap = std::max(0, count(m_row, s) - 1);

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
        if (a == A::Right || a == A::Enter) {
            Row r = neighbour(Row::Outbox, 1, s);
            if (count(Row::Outbox, s) > 0)
                r = Row::Outbox;
            go(r);
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
    if (a == A::Left) {
        if (m_row == Row::Text || m_col == 0) go(Row::List);
        else go(m_row, m_col - 1);
        return {};
    }
    if (a == A::Right) {
        if (m_row != Row::Text) go(m_row, std::min(m_col + 1, cap));
        return {};
    }
    if (m_row == Row::Text) {
        switch (a) {
        case A::Up:
            if (s.textAtTop) go(neighbour(m_row, -1, s));
            else return cmd(Cmd::ScrollLines, 0, -1);
            return {};
        case A::Down: return cmd(Cmd::ScrollLines, 0, 1);
        case A::Space:
        case A::PageDown: return cmd(Cmd::ScrollPages, 0, 1);
        case A::PageUp: return cmd(Cmd::ScrollPages, 0, -1);
        case A::Home: return cmd(Cmd::ScrollTop);
        case A::End: return cmd(Cmd::ScrollEnd);
        default: return {};
        }
    }
    if (a == A::Up || a == A::Down) {
        go(neighbour(m_row, dir, s));
        return {};
    }
    switch (m_row) {
    case Row::Attachments:
        if (a == A::Enter) return cmd(Cmd::OpenAttachment, m_col);
        if (a == A::Space) return cmd(Cmd::PreviewAttachment, m_col);
        if (a == A::Save) return cmd(Cmd::SaveAttachment, m_col);
        break;
    case Row::Links:
        if (a == A::Enter) return cmd(Cmd::OpenLink, m_col);
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
