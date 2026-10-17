#pragma once
// The open letter, painted from the litehtml document with the theme's colours; scrolling,
// selection, link cursor and keyboard link navigation.
#include "Letter.h"
#include "Theme.h"
#include <QQuickPaintedItem>

namespace om {


class LetterView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(om::Letter *letter READ letter WRITE setLetter NOTIFY letterChanged)
    Q_PROPERTY(om::Theme *theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(qreal scrollY READ scrollY WRITE setScrollY NOTIFY scrollYChanged)
    Q_PROPERTY(int contentHeight READ contentHeight NOTIFY contentHeightChanged)
    Q_PROPERTY(int currentLink READ currentLink WRITE setCurrentLink NOTIFY currentLinkChanged)
    Q_PROPERTY(bool atTop READ atTop NOTIFY scrollYChanged)
public:
    explicit LetterView(QQuickItem *parent = nullptr);

    Letter *letter() const { return m_letter; }
    Theme *theme() const { return m_theme; }
    qreal scrollY() const { return m_scrollY; }
    int contentHeight() const { return m_contentHeight; }
    int currentLink() const { return m_currentLink; }
    bool atTop() const { return m_scrollY <= 0; }
    void setLetter(Letter *letter);
    void setTheme(Theme *theme);
    void setScrollY(qreal y);
    void setCurrentLink(int i);

    Q_INVOKABLE void scrollLines(int n);
    Q_INVOKABLE void scrollPages(int n);
    Q_INVOKABLE void toTop() { setScrollY(0); }
    Q_INVOKABLE void toEnd() { setScrollY(m_contentHeight); }
    Q_INVOKABLE QString selectedText() const;
    Q_INVOKABLE void openCurrentLink();

    void paint(QPainter *painter) override;

Q_SIGNALS:
    void letterChanged();
    void themeChanged();
    void scrollYChanged();
    void contentHeightChanged();
    void currentLinkChanged();

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void hoverMoveEvent(QHoverEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void geometryChange(const QRectF &now, const QRectF &old) override;

private:
    int margin() const;
    void documentChanged(bool sameLetter);
    void applyFilter();
    void mouse(const QPointF &pos, int kind, Qt::MouseButton button);

    Letter *m_letter = nullptr;
    Theme *m_theme = nullptr;
    qreal m_scrollY = 0;
    int m_contentHeight = 0;
    int m_currentLink = -1;
};

} // namespace om
