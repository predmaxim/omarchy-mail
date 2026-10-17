#include "Client.h"
#include "FakeMaild.h"
#include "Images.h"
#include "Launch.h"
#include "LaunchStub.h"
#include "Letter.h"
#include "LetterView.h"
#include "Renderer.h"
#include "Theme.h"
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>

using namespace om;
using namespace om::test;

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static QString fixture(const QString &name) { return QStringLiteral(OM_SOURCE_DIR "/tests/fixtures/") + name; }

class TstLetterView : public QObject {
    Q_OBJECT
    FakeMaild *d = nullptr;
    Client *c = nullptr;
    QTemporaryDir *imgDir = nullptr;
    Images *images = nullptr;
    Renderer *renderer = nullptr;
    Theme *theme = nullptr;
    Letter *letter = nullptr;
    QQuickWindow *win = nullptr;
    LetterView *view = nullptr;

    // Shows `html` as letter 42 and waits for the first layout.
    void open(const QString &html)
    {
        d->on("message", [html](const QJsonObject &req) {
            QJsonObject f = QJsonDocument::fromJson(readFile(fixture("message.json"))).object();
            QJsonObject s = f.value("summary").toObject();
            s.insert("id", req.value("message").toInteger());
            f.insert("summary", s);
            f.insert("html", html);
            return QJsonObject{{"result", f}};
        });
        QSignalSpy docs(letter, &Letter::documentChanged);
        letter->show({{"id", 42}});
        QTRY_VERIFY_WITH_TIMEOUT(docs.size() >= 1, 10000);
        QVERIFY(view->contentHeight() > 0);
    }
    // Lays the open letter out again at `z` and waits for it.
    void zoomTo(qreal z)
    {
        QSignalSpy docs(letter, &Letter::documentChanged);
        letter->setZoom(z);
        QTRY_VERIFY_WITH_TIMEOUT(docs.size() >= 1, 10000);
        QCOMPARE(docs.last().at(0).toBool(), true);
        QCOMPARE(letter->documentZoom(), z);
    }
    // Pixels in `r` within a small distance of `c`.
    static int near(const QImage &img, const QRect &r, const QColor &c)
    {
        int n = 0;
        for (int y = qMax(0, r.top()); y <= qMin(img.height() - 1, r.bottom()); ++y)
            for (int x = qMax(0, r.left()); x <= qMin(img.width() - 1, r.right()); ++x) {
                const QColor p = img.pixelColor(x, y);
                n += qAbs(p.red() - c.red()) + qAbs(p.green() - c.green()) + qAbs(p.blue() - c.blue()) < 60;
            }
        return n;
    }
    void wheel(int angle, Qt::KeyboardModifiers mods)
    {
        const QPointF at(400, 300);
        QWheelEvent e(at, win->mapToGlobal(at), QPoint(), QPoint(0, angle), Qt::NoButton, mods, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(win, &e);
    }

private Q_SLOTS:
    void init()
    {
        launch::setProgramForTests("/nonexistent/uwsm-app");
        d = new FakeMaild;
        QVERIFY(d->start());
        c = new Client(d->path());
        c->start();
        QTRY_VERIFY_WITH_TIMEOUT(c->connected(), 2000);
        imgDir = new QTemporaryDir(scratch());
        images = new Images(imgDir->path());
        renderer = new Renderer;
        theme = new Theme;
        letter = new Letter(c, images, renderer, theme);
        win = new QQuickWindow;
        win->resize(800, 600);
        view = new LetterView(win->contentItem());
        view->setSize(QSizeF(800, 600));
        view->setTheme(theme);
        view->setLetter(letter);
        win->show();
        QVERIFY(QTest::qWaitForWindowExposed(win));
    }
    void cleanup()
    {
        delete win;   // before the letter: the view lets go of its document
        delete letter;
        delete renderer;
        delete images;
        delete imgDir;
        delete theme;
        delete c;
        delete d;
    }
    void paints_theme_background()
    {
        open(QString::fromUtf8(readFile(fixture("light.html"))));
        QImage img;
        QTRY_VERIFY_WITH_TIMEOUT((img = win->grabWindow()).pixelColor(799, 599) == theme->background(), 3000);
        int near = 0;
        const QColor fg = theme->foreground();
        for (int y = 0; y < 300; ++y)
            for (int x = 0; x < 800; ++x) {
                const QColor p = img.pixelColor(x, y);
                near += qAbs(p.red() - fg.red()) + qAbs(p.green() - fg.green()) + qAbs(p.blue() - fg.blue()) < 90;
            }
        QVERIFY2(near > 50, qPrintable(QString::number(near)));
    }
    void scroll_clamped()
    {
        open(QStringLiteral("<div style=\"height:3000px\">x</div>"));
        view->setScrollY(1e6);
        QCOMPARE(view->scrollY(), qreal(view->contentHeight() - 600));
        view->setScrollY(-5);
        QCOMPARE(view->scrollY(), qreal(0));
        QVERIFY(view->atTop());
    }
    void current_link_scrolls_into_view()
    {
        open(QStringLiteral("<div style=\"height:3000px\">x</div><a href=\"https://x.org/\">far</a>"));
        view->setCurrentLink(0);
        const QRect r = letter->links().at(0).rect;
        QVERIFY(r.top() >= view->scrollY());
        QVERIFY(r.bottom() <= view->scrollY() + 600);
        QVERIFY(view->scrollY() > 0);
    }
    void click_on_link_activates()
    {
        QTemporaryDir dir(scratch());
        const QString stub = writeStub(dir.path());
        launch::setProgramForTests(stub);
        open(QStringLiteral("<p><a href=\"https://x.org/go\">link text</a></p>"));
        const QRect r = letter->links().at(0).rect;
        const QPoint at = r.center() + QPoint(int(theme->space(18)), -int(view->scrollY()));
        QTest::mouseClick(win, Qt::LeftButton, {}, at);
        QTRY_COMPARE(stubCalls(stub), (QList<QStringList>{{"--", "xdg-open", "https://x.org/go"}}));
    }
    void drag_selects_text()
    {
        open(QStringLiteral("<p>Some selectable words in a line</p>"));
        const int m = theme->space(18);
        QTest::mousePress(win, Qt::LeftButton, {}, QPoint(m + 2, m + 8));
        QTest::mouseMove(win, QPoint(m + 60, m + 8));
        QTest::mouseMove(win, QPoint(m + 120, m + 8));
        QTest::mouseRelease(win, Qt::LeftButton, {}, QPoint(m + 120, m + 8));
        QVERIFY(!view->selectedText().isEmpty());
    }
    void expanding_quotes_keeps_scroll()
    {
        QString lines;
        for (int i = 1; i <= 20; ++i)
            lines += QStringLiteral("Quoted line %1<br>").arg(i);
        open(QStringLiteral("<div style=\"height:800px\">x</div><blockquote>%1</blockquote>").arg(lines));
        const int folded = view->contentHeight();
        view->toEnd();   // the link at the bottom of the view
        const qreal y = view->scrollY();
        QVERIFY(y > 0);
        QCOMPARE(letter->linkCount(), 1);   // "··· Show quoted text (1)"
        QSignalSpy docs(letter, &Letter::documentChanged);
        QTest::mouseClick(win, Qt::LeftButton, {},
                          letter->links().at(0).rect.center() + QPoint(theme->space(18), -int(view->scrollY())));
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 1, 10000);
        QVERIFY(view->contentHeight() > folded);
        QCOMPARE(view->scrollY(), y);
    }
    void zoom_doubles_the_letter()
    {
        open(QStringLiteral("<div style=\"height:1000px\">x</div>"));
        const int h = view->contentHeight();
        zoomTo(2.0);
        QVERIFY2(qAbs(view->contentHeight() - 2 * h) <= 2, qPrintable(QStringLiteral("%1 -> %2").arg(h).arg(view->contentHeight())));
        zoomTo(1.0);
        QCOMPARE(view->contentHeight(), h);
    }
    void zoom_lays_out_at_css_px()   // the layout width, as @media sees it, is the view's width / zoom
    {
        open(QStringLiteral("<style>.w{height:100px}@media (max-width:500px){.w{height:2000px}}</style><div class=w>x</div>"));
        QVERIFY(view->contentHeight() < 500);   // 800 - 2 margins: wider than 500
        zoomTo(2.0);
        QVERIFY2(view->contentHeight() > 4000, qPrintable(QString::number(view->contentHeight())));
    }
    void zoomed_link_is_clicked_and_ringed_where_painted()
    {
        QTemporaryDir dir(scratch());
        const QString stub = writeStub(dir.path());
        launch::setProgramForTests(stub);
        open(QStringLiteral("<div style=\"height:1000px\">x</div><p><a href=\"https://x.org/go\">link text</a></p>"));
        zoomTo(2.0);
        view->setCurrentLink(0);   // scrolled into view: the link is at ~2000 px now
        const QRect r = letter->links().at(0).rect;
        const qreal y = view->scrollY();
        QVERIFY(r.top() * 2 >= y);
        QVERIFY(r.bottom() * 2 <= y + 600);
        const int m = theme->space(18);
        const QPoint mid(m + r.center().x() * 2, int(r.center().y() * 2 - y));
        const QRect edge(mid.x() - 2, int(r.top() * 2 - y) - 3, 5, 6);   // the ring's top: ×2 from the document box
        QTRY_VERIFY_WITH_TIMEOUT(near(win->grabWindow(), edge, theme->accent()) > 0, 3000);
        view->setCurrentLink(-1);
        QTRY_COMPARE_WITH_TIMEOUT(near(win->grabWindow(), edge, theme->accent()), 0, 3000);
        QTest::mouseClick(win, Qt::LeftButton, {}, mid);
        QTRY_COMPARE(stubCalls(stub), (QList<QStringList>{{"--", "xdg-open", "https://x.org/go"}}));
    }
    void fixed_width_letter_shrinks_to_fit()   // W41: never past the right edge; @media sees the width it's shown at
    {
        QTemporaryDir dir(scratch());
        const QString stub = writeStub(dir.path());
        launch::setProgramForTests(stub);
        const int m = theme->space(18), pane = 600;
        view->setSize(QSizeF(pane + 2 * m, 600));
        open(QStringLiteral("<style>.m{height:50px}@media (max-width:500px){.m{height:3000px}}</style>"
                            "<table width=\"600\" cellpadding=\"0\" cellspacing=\"0\" style=\"background:#c00\"><tr><td>"
                            "<div class=\"m\"></div><a href=\"https://x.org/far\" style=\"float:right\">far link</a>"
                            "</td></tr></table>"));
        QSignalSpy docs(letter, &Letter::documentChanged);
        letter->setZoom(2.0);
        QTRY_VERIFY_WITH_TIMEOUT(docs.size() >= 1, 10000);
        QCOMPARE(letter->zoom(), 2.0);   // the reader's zoom stays; the letter is shown smaller
        const qreal z = letter->documentZoom();
        const auto doc = letter->document();
        QVERIFY2(z < 1.0, qPrintable(QString::number(z)));
        QVERIFY2(doc->documentWidth() * z <= pane, qPrintable(QString::number(doc->documentWidth() * z)));
        QCOMPARE(view->contentHeight(), int(std::ceil(doc->documentHeight() * z)));
        QVERIFY2(view->contentHeight() < 1000, qPrintable(QString::number(view->contentHeight())));   // not the 300 px @media
        QImage img;
        QTRY_VERIFY_WITH_TIMEOUT((img = win->grabWindow()).pixelColor(m + pane - 20, int(30 * z)) != theme->background(), 3000);
        for (int x = m + pane + 1; x < pane + 2 * m; ++x)   // the right margin: nothing painted there
            QCOMPARE(img.pixelColor(x, int(30 * z)), theme->background());
        const QRect r = letter->links().at(0).rect;
        QTest::mouseClick(win, Qt::LeftButton, {}, QPoint(m + int(r.center().x() * z), int(r.center().y() * z - view->scrollY())));
        QTRY_COMPARE(stubCalls(stub), (QList<QStringList>{{"--", "xdg-open", "https://x.org/far"}}));
    }
    void text_keeps_the_full_zoom()   // W41: only fixed-width content is shrunk, and not past 1200 CSS px of it
    {
        view->setSize(QSizeF(600 + 2 * theme->space(18), 600));
        letter->setZoom(2.0);
        open(QStringLiteral("<p>Only text: nothing of a fixed width.</p>"));
        QCOMPARE(letter->documentZoom(), 2.0);
        letter->clear();
        open(QStringLiteral("<p>%1</p>").arg(QString(20000, QLatin1Char('x'))));   // its tail is one huge piece
        QVERIFY(letter->document()->documentWidth() > 1200);
        QCOMPARE(letter->documentZoom(), 2.0);   // cut on the right, as before, rather than shrunk to dust
    }
    void zoom_keeps_the_place_in_the_letter()
    {
        open(QStringLiteral("<div style=\"height:3000px\">x</div>"));
        view->setScrollY(1000);
        const qreal at = view->scrollY() / view->contentHeight();
        zoomTo(2.0);
        QVERIFY2(qAbs(view->scrollY() / view->contentHeight() - at) < 0.01, qPrintable(QString::number(view->scrollY())));
    }
    void zoomed_scroll_steps()
    {
        open(QStringLiteral("<div style=\"height:5000px\">x</div>"));
        view->scrollLines(1);
        const qreal line = view->scrollY();
        view->toTop();
        zoomTo(2.0);
        view->scrollLines(1);
        QCOMPARE(view->scrollY(), 2 * line);
    }
    void ctrl_wheel_zooms()
    {
        QTemporaryDir config(scratch());   // the zoom is saved: not into the shared test home
        const QByteArray old = qgetenv("XDG_CONFIG_HOME");
        const auto restore = qScopeGuard([old] { qputenv("XDG_CONFIG_HOME", old); });
        qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
        open(QStringLiteral("<div style=\"height:3000px\">x</div>"));
        QSignalSpy notes(letter, &Letter::note);
        wheel(120, Qt::ControlModifier);
        QCOMPARE(letter->zoom(), 1.1);
        QCOMPARE(view->scrollY(), qreal(0));   // zoomed, not scrolled
        QCOMPARE(notes.last().at(0).toString(), QString("Letter zoom 110%"));
        wheel(60, Qt::ControlModifier);   // a touchpad's half notch: not yet
        QCOMPARE(letter->zoom(), 1.1);
        wheel(60, Qt::ControlModifier);
        QCOMPARE(letter->zoom(), 1.1 * 1.1);
        wheel(-240, Qt::ControlModifier);
        QCOMPARE(letter->zoom(), 1.0);
        wheel(-120, Qt::NoModifier);   // a plain wheel scrolls
        QVERIFY(view->scrollY() > 0);
        QCOMPARE(letter->zoom(), 1.0);
    }
    void rerender_keeps_scroll()
    {
        open(QStringLiteral("<div style=\"height:3000px\">x</div>"));
        view->setScrollY(200);
        Q_EMIT letter->documentChanged(true);
        QCOMPARE(view->scrollY(), qreal(200));
        view->setCurrentLink(-1);
        Q_EMIT letter->documentChanged(false);
        QCOMPARE(view->scrollY(), qreal(0));
        QCOMPARE(view->currentLink(), -1);
    }
};

QTEST_MAIN(TstLetterView)
#include "tst_letterview.moc"
