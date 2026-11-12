// App's keys: the window, the search field and the form; the Esc chain and Nav's steps.
#include "App.h"
#include "Compose.h"
#include "Letter.h"
#include "MailList.h"
#include <QScopedValueRollback>
#include <QTransform>

namespace om {

namespace {
// A held key repeats moving and scrolling only.
bool repeats(KeyAction a)
{
    using A = KeyAction;
    return a == A::Up || a == A::Down || a == A::Left || a == A::Right || a == A::PageUp || a == A::PageDown
        || a == A::Space;
}
} // namespace

bool App::key(int key, quint32 nativeScanCode, int modifiers, bool autoRepeat, const QString &focus)
{
    const QScopedValueRollback<bool> user(m_byUser, true);
    const KeyPress k = mapKey(key, nativeScanCode, Qt::KeyboardModifiers::fromInt(modifiers));
    if (k.action == KeyAction::None)
        return false;
    if (k.action == KeyAction::ZoomIn || k.action == KeyAction::ZoomOut || k.action == KeyAction::ZoomReset) {
        // the window root and the search field; not the form or the mailboxes page, where no letter is shown
        if (focus == QLatin1String("compose") || m_page != QLatin1String("main") || m_compose->isOpen())
            return false;
        m_letter->zoomBy(k.action == KeyAction::ZoomIn ? 1 : k.action == KeyAction::ZoomOut ? -1 : 0);
        return true;
    }
    if (focus == QLatin1String("compose"))
        return composeKey(k, autoRepeat);
    if (focus == QLatin1String("search"))
        return searchKey(k, autoRepeat);
    return focus.isEmpty() && windowKey(k, autoRepeat);
}

// Letter commands work on any row; moving, Enter, Space and `s` go through Nav.
bool App::windowKey(const KeyPress &k, bool repeat)
{
    using A = KeyAction;
    const A a = k.action;
    if (m_page != QLatin1String("main")) {   // the mailbox page has keys of its own
        if (a != A::Escape)
            return false;
        if (!repeat)
            closeSettings();
        return true;
    }
    if (m_compose->isOpen())
        return composeKey(k, repeat);   // the list is disabled under the form: a stray `d` deletes nothing
    if (a == A::CopySelection || a == A::Send || a == A::Paste || a == A::Attach || a == A::Discard)
        return false;   // Ctrl+C: QML copies the letter's selected text; the rest are the form's
    if (repeat && !repeats(a))
        return true;   // a held `d` in Trash is not the second press
    switch (a) {
    case A::Compose: composeNew(); break;
    case A::Reply: reply(); break;
    case A::Remove: remove(); break;
    case A::ToggleRead: toggleRead(); break;
    case A::ToggleFlag: toggleFlag(); break;
    case A::Original: openOriginal(); break;
    case A::Copy: copyText(); break;
    case A::Search:
        m_nav->reset(Row::Search);
        Q_EMIT focusSearch();
        break;
    case A::PickAccount:   // one mailbox: no mailbox picker
        if (!accountChoices().isEmpty())
            Q_EMIT openPicker(QStringLiteral("account"));
        break;
    case A::PickFolder: Q_EMIT openPicker(QStringLiteral("folder")); break;
    case A::UnreadFilter: toggleUnread(); break;
    case A::Undo: undo(); break;
    case A::SelectAll: m_list->selectAll(); break;
    case A::Mark: m_list->toggleMark(); break;
    case A::Escape: back(); break;
    default: run(m_nav->press(k, shape()), repeat); break;
    }
    return true;
}

// The field keeps typing and moving in it; ↓ and Enter go to the first row, Esc clears, then leaves for the pickers.
bool App::searchKey(const KeyPress &k, bool repeat)
{
    using A = KeyAction;
    if (k.action != A::Down && k.action != A::Enter && k.action != A::Escape)
        return false;
    if (repeat && k.action != A::Down)
        return true;
    if (k.action == A::Escape && !m_query.isEmpty()) {
        setQuery({});   // back to the folder, the field keeps the keys
        return true;
    }
    if (k.action != A::Escape) {
        m_list->setCursor(0);
        m_nav->reset(Row::List);
        m_readTimer.start();   // the user went to the results: the letter on row 0 is theirs now
    } else {
        m_nav->reset(Row::Pickers);
    }
    Q_EMIT leaveSearch();
    return true;
}

// The form keeps every key but its own commands.
bool App::composeKey(const KeyPress &k, bool repeat)
{
    using A = KeyAction;
    const A a = k.action;
    if (a != A::Send && a != A::Paste && a != A::Attach && a != A::Discard && a != A::Escape)
        return false;
    if (repeat)
        return true;   // a held Ctrl+Enter sends once, a held Ctrl+D is not the second press
    if (a == A::Send)
        send();
    else if (a == A::Paste)
        return m_compose->paste();   // no image or files: the field pastes the text
    else if (a == A::Attach)
        Q_EMIT pickFiles();
    else if (a == A::Discard)
        m_compose->discard();
    else
        m_compose->close();
    return true;
}

// Esc, one step at a time, down to the pickers; it never closes the window (an open picker list closes itself in QML).
void App::back()
{
    switch (Row(m_nav->row())) {
    case Row::Pickers:
        break;   // the last step: the key is eaten
    case Row::Actions:
        m_nav->reset(Row::Pickers);
        break;
    case Row::Search:   // the line, the field not focused: like the field, the query first
        if (m_searching)
            setQuery({});
        else
            m_nav->reset(Row::Pickers);
        break;
    case Row::List:
        if (m_list->selectedCount() > 0)
            m_list->clearSelection();
        else if (m_searching)
            setQuery({});
        else
            m_nav->reset(Row::Pickers);   // ↓ comes back to the same letter
        break;
    default:   // the letter's rows
        m_nav->reset(Row::List);
        break;
    }
}

NavShape App::shape() const
{
    NavShape s;
    s.pickers = accountChoices().isEmpty() ? 1 : 2;
    s.outbox = m_letter->outbox() == 2 ? 2 : 0;   // "Send again" and "Discard" under a mail that wasn't sent
    s.attachments = int(m_letter->attachments().size());
    const qreal z = m_letter->documentZoom();   // the mapping LetterView paints with
    for (const DocumentContainer::Link &l : m_letter->links())
        s.links << QTransform::fromScale(z, z).mapRect(QRectF(l.rect));
    s.view = m_letterViewport;
    s.listAtTop = m_list->cursor() <= 0;
    return s;
}

void App::setLetterViewport(const QRectF &v)
{
    m_letterViewport = v;
    m_nav->scrolled(shape());
}

void App::run(const Step &st, bool repeat)
{
    switch (st.cmd) {
    case Cmd::ListMove:
        m_list->move(st.dir, st.ctrl, st.shift);
        return;
    case Cmd::ListEdge:
        m_list->move(st.dir * m_list->rowCount(), st.ctrl, st.shift);   // move clamps to the first / last loaded row
        return;
    case Cmd::ScrollLines:
    case Cmd::ScrollPages:
    case Cmd::ScrollTop:
    case Cmd::ScrollEnd:
        Q_EMIT scroll(int(st.cmd), st.dir);
        return;
    default:
        break;
    }
    if (repeat)
        return;   // a held Space pages through the letter, it doesn't preview an attachment again and again
    switch (st.cmd) {
    case Cmd::FocusSearch: Q_EMIT focusSearch(); break;
    case Cmd::OpenPicker:
        Q_EMIT openPicker(st.col == 0 && !accountChoices().isEmpty() ? QStringLiteral("account") : QStringLiteral("folder"));
        break;
    case Cmd::PressAction: pressAction(st.col); break;
    case Cmd::OpenAttachment: m_letter->openAttachment(st.col); break;
    case Cmd::PreviewAttachment: m_letter->previewAttachment(st.col); break;
    case Cmd::SaveAttachment: m_letter->saveAttachment(st.col); break;
    case Cmd::OpenLink: m_letter->activateLink(m_letter->links().value(st.col).url); break;
    case Cmd::PressOutbox:
        if (st.col == 0)
            retryOutbox();
        else
            discardOutbox();
        break;
    default:
        break;   // EnterLetter: Nav is in the letter already
    }
}

} // namespace om
