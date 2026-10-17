#include "LetterView.h"

#include "Letter.h"
#include "Recolor.h"
#include "Theme.h"
#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QPainter>
#include <QPointer>
#include <QQuickWindow>

namespace om {

LetterView::LetterView(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
}

int LetterView::margin() const { return m_theme ? m_theme->space(18) : 0; }

void LetterView::setLetter(Letter *letter)
{
    if (letter == m_letter)
        return;
    if (m_letter)
        m_letter->disconnect(this);
    m_letter = letter;
    if (m_letter) {
        connect(m_letter, &Letter::documentChanged, this, &LetterView::documentChanged);
        m_letter->setWidth(int(width()) - 2 * margin());
    }
    Q_EMIT letterChanged();
    documentChanged(false);
}

void LetterView::setTheme(Theme *theme)
{
    if (theme == m_theme)
        return;
    if (m_theme)
        m_theme->disconnect(this);
    m_theme = theme;
    if (m_theme)
        connect(m_theme, &Theme::changed, this, [this] {
            applyFilter();
            update();
        });
    Q_EMIT themeChanged();
    applyFilter();
    if (m_letter)
        m_letter->setWidth(int(width()) - 2 * margin());
    update();
}

void LetterView::applyFilter()
{
    const auto doc = m_letter ? m_letter->document() : nullptr;
    if (!doc || !m_theme)
        return;
    doc->setColorFilter(recolor::filter(m_theme->background(), m_theme->foreground(), m_letter->dark()));
}

void LetterView::documentChanged(bool sameLetter)
{
    const auto doc = m_letter ? m_letter->document() : nullptr;
    if (doc) {
        QPointer<LetterView> self(this);
        doc->setCursorCallback([self](QCursor c) {
            if (self)
                self->setCursor(c);
        });
        doc->setLinkCallback([self](QUrl url) {   // the document itself ignores a release that ends a selection
            if (self && self->m_letter)
                self->m_letter->activateLink(url);
        });
        applyFilter();
    }
    const int h = doc ? doc->documentHeight() : 0;
    if (h != m_contentHeight) {
        m_contentHeight = h;
        Q_EMIT contentHeightChanged();
    }
    if (!sameLetter) {
        if (m_currentLink != -1) {
            m_currentLink = -1;
            Q_EMIT currentLinkChanged();
        }
        m_scrollY = 0;
        Q_EMIT scrollYChanged();
    } else {
        setScrollY(m_scrollY);   // the document may have become shorter
    }
    update();
}

void LetterView::setScrollY(qreal y)
{
    y = qBound(qreal(0), y, qreal(qMax(0, m_contentHeight - int(height()))));
    if (y == m_scrollY)
        return;
    m_scrollY = y;
    Q_EMIT scrollYChanged();
    update();
}

void LetterView::scrollLines(int n)
{
    setScrollY(m_scrollY + n * 3 * (m_theme ? m_theme->fontBody() : 16));
}

void LetterView::scrollPages(int n)
{
    const int body = m_theme ? m_theme->fontBody() : 16;
    setScrollY(m_scrollY + n * (height() - 2 * body));
}

void LetterView::setCurrentLink(int i)
{
    const auto links = m_letter ? m_letter->links() : QList<DocumentContainer::Link>();
    if (i < -1 || i >= links.size())
        i = -1;
    if (i >= 0) {
        const QRect r = links.at(i).rect;
        const int m = margin();
        if (r.top() - m < m_scrollY)
            setScrollY(r.top() - m);
        else if (r.bottom() + m > m_scrollY + height())
            setScrollY(r.bottom() + m - height());
    }
    if (i == m_currentLink)
        return;
    m_currentLink = i;
    Q_EMIT currentLinkChanged();
    update();
}

QString LetterView::selectedText() const
{
    const auto doc = m_letter ? m_letter->document() : nullptr;
    return doc ? doc->selectedText() : QString();
}

void LetterView::copySelection() const
{
    if (const QString text = selectedText(); !text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void LetterView::openCurrentLink()
{
    const auto links = m_letter ? m_letter->links() : QList<DocumentContainer::Link>();
    if (m_currentLink >= 0 && m_currentLink < links.size())
        m_letter->activateLink(links.at(m_currentLink).url);
}

void LetterView::paint(QPainter *p)
{
    p->fillRect(QRectF(0, 0, width(), height()), m_theme ? m_theme->background() : QColor(Qt::black));
    const auto doc = m_letter ? m_letter->document() : nullptr;
    if (!doc || !doc->hasDocument())
        return;
    const int m = margin(), y = int(m_scrollY);
    p->save();
    p->translate(m, 0);
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);
    p->setRenderHint(QPainter::Antialiasing, true);
    doc->setScrollPosition(QPoint(0, y));
    doc->draw(p, QRect(0, 0, int(width()) - m, int(height())));
    p->restore();

    const auto links = m_letter->links();
    if (m_currentLink >= 0 && m_currentLink < links.size() && m_theme) {
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(QPen(m_theme->accent(), 2));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(links.at(m_currentLink).rect.translated(m, -y)), m_theme->space(4), m_theme->space(4));
    }
}

// kind: 0 press, 1 move, 2 release
void LetterView::mouse(const QPointF &pos, int kind, Qt::MouseButton button)
{
    const auto doc = m_letter ? m_letter->document() : nullptr;
    if (!doc || !doc->hasDocument())
        return;
    const QPoint view(int(pos.x()) - margin(), int(pos.y()));
    const QPoint docPos = view + QPoint(0, int(m_scrollY));
    doc->setScrollPosition(QPoint(0, int(m_scrollY)));
    if (kind == 0)
        doc->mousePressEvent(docPos, view, button);
    else if (kind == 1)
        doc->mouseMoveEvent(docPos, view);
    else
        doc->mouseReleaseEvent(docPos, view, button);
    update();   // selection and :hover boxes are small; the window is one repaint
}

void LetterView::mousePressEvent(QMouseEvent *e)
{
    Q_EMIT pressed();
    mouse(e->position(), 0, e->button());
}
void LetterView::mouseMoveEvent(QMouseEvent *e) { mouse(e->position(), 1, Qt::NoButton); }
void LetterView::mouseReleaseEvent(QMouseEvent *e) { mouse(e->position(), 2, e->button()); }
void LetterView::hoverMoveEvent(QHoverEvent *e) { mouse(e->position(), 1, Qt::NoButton); }

void LetterView::wheelEvent(QWheelEvent *e)
{
    const QPoint px = e->pixelDelta();
    setScrollY(m_scrollY - (px.isNull() ? e->angleDelta().y() / 120.0 * 3 * (m_theme ? m_theme->fontBody() : 16) : px.y()));
}

void LetterView::geometryChange(const QRectF &now, const QRectF &old)
{
    QQuickPaintedItem::geometryChange(now, old);
    if (m_letter && now.width() != old.width())
        m_letter->setWidth(int(now.width()) - 2 * margin());
    setScrollY(m_scrollY);
}

} // namespace om
