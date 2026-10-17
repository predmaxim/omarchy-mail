#include "Client.h"
#include "FakeMaild.h"
#include "Images.h"
#include "Launch.h"
#include "LaunchStub.h"
#include "Letter.h"
#include "LetterView.h"
#include "Renderer.h"
#include "Theme.h"
#include <QJsonDocument>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

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
