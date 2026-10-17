#pragma once
#include <QtGlobal>
#include <Qt>

namespace om {

enum class KeyAction {
    None, Up, Down, Left, Right, Enter, Escape, Space, PageUp, PageDown, Home, End,
    Remove, Compose, Reply, ToggleRead, ToggleFlag, Original, Copy, Save, Search,
    UnreadFilter, Undo, SelectAll, Mark, Send, Paste, Attach, Discard, CopySelection, ZoomIn, ZoomOut, ZoomReset
};

struct KeyPress {
    KeyAction action = KeyAction::None;
    bool ctrl = false;
    bool shift = false;
};

// Letters: Latin Qt key code, else evdev scan code + 8 (layout independent).
KeyPress mapKey(int key, quint32 nativeScanCode, Qt::KeyboardModifiers mods);

}
