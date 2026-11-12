#pragma once
#include "Keys.h"
#include <QList>
#include <QObject>
#include <QRectF>
#include <QString>

namespace om {

enum class Row { Actions, Search, Pickers, List, Outbox, Attachments, Body };

struct NavShape {
    int actions = 6;
    int pickers = 2;
    int outbox = 0;
    int attachments = 0;
    QList<QRectF> links;   // the letter's links in view px (document box × documentZoom), document order; an empty one has no box
    QRectF view;           // the letter's part on screen, view px: y = scrollY, height = the view's
    bool listAtTop = false;
};

enum class Cmd {
    None, FocusSearch, OpenPicker, PressAction, ListMove, EnterLetter, OpenAttachment,
    PreviewAttachment, SaveAttachment, OpenLink, PressOutbox, ScrollLines, ScrollPages,
    ScrollTop, ScrollEnd, ListEdge
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
    int col() const { return m_col; }   // on Row::Body: an index into NavShape::links, -1 for no current link
    Step press(const KeyPress &k, const NavShape &s);
    void reset(Row r = Row::List, int col = 0);
    void scrolled(const NavShape &s);   // the letter moved under the cursor: a link wholly off screen, or gone, stops being current
Q_SIGNALS:
    void changed();
private:
    void go(Row r, int col = 0);
    void enter(Row r, const NavShape &s);   // a letter row from another: the body takes its first link on screen
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
