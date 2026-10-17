#include "Keys.h"
#include <QTest>

using namespace om;

static KeyAction act(int key, quint32 sc = 0, Qt::KeyboardModifiers m = {}) { return mapKey(key, sc, m).action; }

class TstKeys : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void latin_letters()
    {
        QCOMPARE(act(Qt::Key_C), KeyAction::Compose);
        QCOMPARE(act(Qt::Key_R), KeyAction::Reply);
        QCOMPARE(act(Qt::Key_D), KeyAction::Remove);
        QCOMPARE(act(Qt::Key_U), KeyAction::ToggleRead);
        QCOMPARE(act(Qt::Key_F), KeyAction::ToggleFlag);
        QCOMPARE(act(Qt::Key_O), KeyAction::Original);
        QCOMPARE(act(Qt::Key_Y), KeyAction::Copy);
        QCOMPARE(act(Qt::Key_S), KeyAction::Save);
        QCOMPARE(act(Qt::Key_Slash), KeyAction::Search);
        QCOMPARE(act(Qt::Key_A), KeyAction::None);
        QCOMPARE(act(Qt::Key_U, 0, Qt::ShiftModifier), KeyAction::UnreadFilter);
        QCOMPARE(act(Qt::Key_C, 0, Qt::ShiftModifier), KeyAction::None);
    }
    void cyrillic_by_scancode()
    {
        QCOMPARE(act(0x0421, 54), KeyAction::Compose);
        QCOMPARE(act(0x041A, 27), KeyAction::Reply);
        QCOMPARE(act(0x0412, 40), KeyAction::Remove);
        QCOMPARE(act(0x0413, 30), KeyAction::ToggleRead);
        QCOMPARE(act(0x0410, 41), KeyAction::ToggleFlag);
        QCOMPARE(act(0x0429, 32), KeyAction::Original);
        QCOMPARE(act(0x041D, 29), KeyAction::Copy);
        QCOMPARE(act(0x042B, 39), KeyAction::Save);
        QCOMPARE(act(0x0413, 30, Qt::ShiftModifier), KeyAction::UnreadFilter);
    }
    void ctrl_combos_both_layouts()
    {
        const Qt::KeyboardModifiers c = Qt::ControlModifier;
        QCOMPARE(act(Qt::Key_Z, 0, c), KeyAction::Undo);
        QCOMPARE(act(0x042F, 52, c), KeyAction::Undo);
        QCOMPARE(act(Qt::Key_A, 0, c), KeyAction::SelectAll);
        QCOMPARE(act(0x0424, 38, c), KeyAction::SelectAll);
        QCOMPARE(act(Qt::Key_V, 0, c), KeyAction::Paste);
        QCOMPARE(act(0x041C, 55, c), KeyAction::Paste);
        QCOMPARE(act(Qt::Key_O, 0, c), KeyAction::Attach);
        QCOMPARE(act(0x0429, 32, c), KeyAction::Attach);
        QCOMPARE(act(Qt::Key_D, 0, c), KeyAction::Discard);
        QCOMPARE(act(0x0412, 40, c), KeyAction::Discard);
        QCOMPARE(act(Qt::Key_C, 0, c), KeyAction::CopySelection);
        QCOMPARE(act(0x0421, 54, c), KeyAction::CopySelection);
        QCOMPARE(act(Qt::Key_Return, 0, c), KeyAction::Send);
        QCOMPARE(act(Qt::Key_Enter, 0, c), KeyAction::Send);
        QCOMPARE(act(Qt::Key_Space, 0, c), KeyAction::Mark);
    }
    void slash_both_layouts()
    {
        QCOMPARE(act(Qt::Key_Slash), KeyAction::Search);
        QCOMPARE(act(Qt::Key_Period, 61), KeyAction::Search);
        QCOMPARE(act(Qt::Key_Period, 60), KeyAction::None);
    }
    void arrows_with_mods()
    {
        const KeyPress d = mapKey(Qt::Key_Down, 0, Qt::ControlModifier);
        QCOMPARE(d.action, KeyAction::Down);
        QVERIFY(d.ctrl);
        QVERIFY(!d.shift);
        const KeyPress u = mapKey(Qt::Key_Up, 0, Qt::ShiftModifier);
        QCOMPARE(u.action, KeyAction::Up);
        QVERIFY(u.shift);
        QVERIFY(!u.ctrl);
        QCOMPARE(act(Qt::Key_Escape), KeyAction::Escape);
        QCOMPARE(act(Qt::Key_PageDown), KeyAction::PageDown);
        QCOMPARE(act(Qt::Key_Home), KeyAction::Home);
        QCOMPARE(act(Qt::Key_Space), KeyAction::Space);
        QCOMPARE(act(Qt::Key_Return), KeyAction::Enter);
    }
    void alt_meta_not_ours()
    {
        QCOMPARE(act(Qt::Key_D, 0, Qt::AltModifier), KeyAction::None);
        QCOMPARE(act(Qt::Key_C, 0, Qt::MetaModifier), KeyAction::None);
    }
    void zoom_keys()
    {
        const Qt::KeyboardModifiers c = Qt::ControlModifier, keypad = c | Qt::KeypadModifier;
        QCOMPARE(act(Qt::Key_Equal, 21, c), KeyAction::ZoomIn);
        QCOMPARE(act(Qt::Key_Plus, 21, c | Qt::ShiftModifier), KeyAction::ZoomIn);
        QCOMPARE(act(Qt::Key_Plus, 86, keypad), KeyAction::ZoomIn);
        QCOMPARE(act(Qt::Key_Minus, 20, c), KeyAction::ZoomOut);
        QCOMPARE(act(Qt::Key_Minus, 82, keypad), KeyAction::ZoomOut);
        QCOMPARE(act(Qt::Key_0, 19, c), KeyAction::ZoomReset);
        QCOMPARE(act(Qt::Key_0, 90, keypad), KeyAction::ZoomReset);
        QCOMPARE(act(Qt::Key_Agrave, 19, c), KeyAction::ZoomReset);   // a layout without the sign there: by its key
        QCOMPARE(act(Qt::Key_Equal, 21), KeyAction::None);
        QCOMPARE(act(Qt::Key_Minus, 20), KeyAction::None);
        QCOMPARE(act(Qt::Key_0, 19), KeyAction::None);
    }
    void delete_key()
    {
        QCOMPARE(act(Qt::Key_Delete), KeyAction::Remove);
        QCOMPARE(act(Qt::Key_Delete, 0, Qt::ShiftModifier), KeyAction::None);
    }
};

QTEST_MAIN(TstKeys)
#include "tst_keys.moc"
