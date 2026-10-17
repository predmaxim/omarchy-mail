#include "Renderer.h"

#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace om;

static QString fixture(const QString &name)
{
    return QStringLiteral(OM_SOURCE_DIR "/tests/fixtures/") + name;
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static bool isRed(QRgb p)
{
    return qRed(p) > 200 && qGreen(p) < 60 && qBlue(p) < 60;
}

static QImage drawn(const Rendered &r)
{
    QImage img(760, qMax(1, qMin(r.size.height(), 2000)), QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    r.doc->draw(&p, img.rect());
    return img;
}

static int redPixels(const QImage &img)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            n += isRed(img.pixel(x, y));
    return n;
}

class TstRenderer : public QObject
{
    Q_OBJECT
    QTemporaryDir m_images; // empty image cache unless a test fills it

    RenderJob job(const QString &html, quint64 generation = 1) const
    {
        RenderJob j;
        j.generation = generation;
        j.html = html;
        j.imageDir = m_images.path();
        j.width = 760;
        j.viewportHeight = 600;
        j.font = QFont(QStringLiteral("sans-serif"));
        j.font.setPixelSize(16);
        j.selection = QColor(QStringLiteral("#3366cc"));
        return j;
    }

    static Rendered run(const RenderJob &j, int timeoutMs = 5000)
    {
        Renderer r;
        QSignalSpy spy(&r, &Renderer::rendered);
        r.render(j);
        if (!spy.wait(timeoutMs))
            return {};
        return spy.first().first().value<Rendered>();
    }

private Q_SLOTS:
    void renders_light_fixture()
    {
        const Rendered r = run(job(QString::fromUtf8(readFile(fixture("light.html")))));
        QVERIFY(r.doc);
        QCOMPARE(r.generation, 1u);
        QVERIFY(r.size.height() > 0);
        QVERIFY(!r.dark);
        QVERIFY(r.wanted.isEmpty());
        QCOMPARE(r.links.size(), 2);
        QCOMPARE(r.links[0].url, QUrl("https://example.org/digest?week=42"));
        QCOMPARE(r.links[1].url, QUrl("https://example.org/unsubscribe?u=alex"));
        QVERIFY(!r.links[0].rect.isEmpty());
        QVERIFY(r.links[0].rect.top() < r.links[1].rect.top());
    }

    void detects_dark_fixture()
    {
        const Rendered r = run(job(QString::fromUtf8(readFile(fixture("dark.html")))));
        QVERIFY(r.doc);
        QVERIFY(r.dark);
    }

    void cid_from_inline_map()
    {
        RenderJob j = job(QString::fromUtf8(readFile(fixture("cid.html"))));
        j.inlinePaths.insert(QStringLiteral("logo@x"), fixture("logo.png"));
        const Rendered r = run(j);
        QVERIFY(r.doc);
        QVERIFY(r.wanted.isEmpty());
        QVERIFY(isRed(drawn(r).pixel(5, 5)));
    }

    void remote_urls_wanted()
    {
        const QString url = QStringLiteral("https://example.invalid/a.png");
        const QString html = QStringLiteral("<body style=\"margin:0\"><img src=\"%1\" style=\"display:block\"></body>").arg(url);
        const Rendered r = run(job(html));
        QVERIFY(r.doc);
        QCOMPARE(r.wanted, QStringList{url});

        // once the file is in the cache it is drawn and no longer wanted
        QFile f(m_images.path() + "/" + QCryptographicHash::hash(QUrl(url).toEncoded(), QCryptographicHash::Sha1).toHex());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(readFile(fixture("logo.png")));
        f.close();
        const Rendered again = run(job(html));
        f.remove();
        QVERIFY(again.doc);
        QVERIFY(again.wanted.isEmpty());
        QVERIFY(isRed(drawn(again).pixel(5, 5)));
    }

    void data_url_image()
    {
        const QString b64 = QString::fromLatin1(readFile(fixture("logo.png")).toBase64());
        const Rendered r = run(job(
            QStringLiteral("<body style=\"margin:0\"><img src=\"data:image/png;base64,%1\" style=\"display:block\"></body>").arg(b64)));
        QVERIFY(r.doc);
        QVERIFY(r.wanted.isEmpty());
        QVERIFY(isRed(drawn(r).pixel(5, 5)));
    }

    void load_data_schemes()
    {
        const RenderJob j = job({});
        QStringList wanted;
        QCOMPARE(loadData(QUrl("data:image/x-test,a%20b"), j, &wanted), QByteArray("a b"));
        QCOMPARE(loadData(QUrl("data:text/html,x"), j, &wanted), QByteArray());
        QCOMPARE(loadData(QUrl("file://" OM_SOURCE_DIR "/tests/fixtures/logo.png"), j, &wanted), QByteArray());
        QCOMPARE(loadData(QUrl("cid:missing@x"), j, &wanted), QByteArray());
        QCOMPARE(loadData(QUrl("ftp://example.invalid/a.png"), j, &wanted), QByteArray());
        QVERIFY(wanted.isEmpty());
    }

    void color_filter_applied()
    {
        const Rendered r = run(job(QString::fromUtf8(readFile(fixture("light.html")))));
        QVERIFY(r.doc);
        QCOMPARE(redPixels(drawn(r)), 0);
        r.doc->setColorFilter([](const QColor &) { return QColor(Qt::red); });
        QVERIFY(redPixels(drawn(r)) > 0);
    }

    void stale_generation_dropped()
    {
        const QString light = QString::fromUtf8(readFile(fixture("light.html")));
        const QString heavy = light.repeated(2000);
        Renderer r;
        QList<quint64> got;
        connect(&r, &Renderer::rendered, this, [&](const Rendered &x) { got << x.generation; });
        r.render(job(heavy, 1));
        r.render(job(light, 2));
        QTRY_VERIFY_WITH_TIMEOUT(!got.isEmpty(), 10000);
        QCoreApplication::processEvents(); // one thread, queued in order: a stale result would have come first
        QCOMPARE(got, QList<quint64>{2});

        // generation 3 is being laid out when 4 comes: it finishes but is not delivered
        r.render(job(light.repeated(100), 3));
        QTest::qWait(20);
        r.render(job(light, 4));
        QTRY_COMPARE_WITH_TIMEOUT(got.size(), 2, 10000);
        QCoreApplication::processEvents();
        QCOMPARE(got, (QList<quint64>{2, 4}));
    }

    void garbage_html_renders()
    {
        const Rendered nested = run(job(QStringLiteral("<table><tr><td><div><b><i>").repeated(5000)), 30000);
        QVERIFY(nested.doc);
        const Rendered word = run(job(QString(1 << 20, QLatin1Char('x'))), 30000);
        QVERIFY(word.doc);
    }
};

QTEST_MAIN(TstRenderer)
#include "tst_renderer.moc"
