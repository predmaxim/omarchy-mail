#pragma once
#include "Keys.h"
#include <QObject>
#include <QString>

namespace om {

enum class Row { Actions, Search, Pickers, List, Outbox, Attachments, Links, Text };

struct NavShape {
    int actions = 5;
    int pickers = 2;
    int outbox = 0;
    int attachments = 0;
    int links = 0;
    bool listAtTop = false;
    bool textAtTop = true;
};

enum class Cmd {
    None, FocusSearch, OpenPicker, PressAction, ListMove, EnterLetter, OpenAttachment,
    PreviewAttachment, SaveAttachment, OpenLink, PressOutbox, ScrollLines, ScrollPages,
    ScrollTop, ScrollEnd
};

struct Step {
    Cmd cmd = Cmd::None;
    int col = 0;
    int dir = 0;
    bool ctrl = false;
    bool shift = false;
};

class Nav : public QObject {
    Q_OBJECT
    Q_PROPERTY(int row READ row NOTIFY changed)
    Q_PROPERTY(int col READ col NOTIFY changed)
public:
    using QObject::QObject;
    int row() const { return int(m_row); }
    int col() const { return m_col; }
    Step press(const KeyPress &k, const NavShape &s);
    void reset(Row r = Row::List);
Q_SIGNALS:
    void changed();
private:
    void go(Row r, int col = 0);
    Row m_row = Row::List;
    int m_col = 0;
};

// Two presses of the same key within 3 s confirm a destructive command.
struct Confirm {
    QString armed;
    qint64 at = 0;
    bool press(const QString &what, qint64 nowMs);
};

}
