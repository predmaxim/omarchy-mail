#include "Keys.h"

namespace om {

namespace {

// Returns 'a'..'z' or 0.
char letter(int key, quint32 sc)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return char('a' + (key - Qt::Key_A));
    if (key < 0x80)
        return 0;
    switch (sc) {
    case 54: return 'c';
    case 27: return 'r';
    case 40: return 'd';
    case 30: return 'u';
    case 41: return 'f';
    case 32: return 'o';
    case 29: return 'y';
    case 39: return 's';
    case 52: return 'z';
    case 38: return 'a';
    case 55: return 'v';
    }
    return 0;
}

}

KeyPress mapKey(int key, quint32 sc, Qt::KeyboardModifiers mods)
{
    if (mods & (Qt::AltModifier | Qt::MetaModifier))
        return {};
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    auto k = [&](KeyAction a) { return KeyPress{a, ctrl, shift}; };

    if (ctrl) {
        if (key == Qt::Key_Return || key == Qt::Key_Enter)
            return {KeyAction::Send, true, shift};
        if (key == Qt::Key_Space)
            return {KeyAction::Mark, true, shift};
    }
    switch (key) {
    case Qt::Key_Up: return k(KeyAction::Up);
    case Qt::Key_Down: return k(KeyAction::Down);
    case Qt::Key_Left: return k(KeyAction::Left);
    case Qt::Key_Right: return k(KeyAction::Right);
    case Qt::Key_Return:
    case Qt::Key_Enter: return k(KeyAction::Enter);
    case Qt::Key_Escape: return k(KeyAction::Escape);
    case Qt::Key_Space: return k(KeyAction::Space);
    case Qt::Key_PageUp: return k(KeyAction::PageUp);
    case Qt::Key_PageDown: return k(KeyAction::PageDown);
    case Qt::Key_Home: return k(KeyAction::Home);
    case Qt::Key_End: return k(KeyAction::End);
    case Qt::Key_Delete: return (ctrl || shift) ? KeyPress{} : KeyPress{KeyAction::Remove};
    }
    if (key == Qt::Key_Slash || (key == Qt::Key_Period && sc == 61))
        return (ctrl || shift) ? KeyPress{} : KeyPress{KeyAction::Search};

    const char c = letter(key, sc);
    if (ctrl) {
        KeyAction a = KeyAction::None;
        switch (c) {
        case 'z': a = KeyAction::Undo; break;
        case 'a': a = KeyAction::SelectAll; break;
        case 'v': a = KeyAction::Paste; break;
        case 'o': a = KeyAction::Attach; break;
        case 'd': a = KeyAction::Discard; break;
        case 'c': a = KeyAction::CopySelection; break;
        }
        return {a, true, shift};
    }
    if (shift)
        return c == 'u' ? KeyPress{KeyAction::UnreadFilter, false, true} : KeyPress{};
    switch (c) {
    case 'c': return {KeyAction::Compose};
    case 'r': return {KeyAction::Reply};
    case 'd': return {KeyAction::Remove};
    case 'u': return {KeyAction::ToggleRead};
    case 'f': return {KeyAction::ToggleFlag};
    case 'o': return {KeyAction::Original};
    case 'y': return {KeyAction::Copy};
    case 's': return {KeyAction::Save};
    }
    return {};
}

}
