#include "Renderer.h"
#include "Letter.h"

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

    void meta_charset_ignored()
    {
        // the letter comes decoded: its own charset must not decode it a second time
        const QString metas[] = {QStringLiteral("<meta charset=\"windows-1251\">"),
                                 QStringLiteral("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=koi8-r\">")};
        for (const QString &meta : metas) {
            const Rendered r = run(job(QStringLiteral("<!doctype html><html><head>%1</head><body><p>Привет</p></body></html>").arg(meta)));
            QVERIFY(r.doc);
            bool found = false;
            r.doc->findText(QStringLiteral("Привет"), {}, false, nullptr, &found, nullptr, nullptr);
            QVERIFY2(found, qPrintable(meta));
        }
    }

    void external_css_ignored()
    {
        const Rendered r = run(job(QStringLiteral(
            "<link rel=\"stylesheet\" href=\"https://example.invalid/s.css\">"
            "<style>@import url(https://example.invalid/i.css);</style><p>Hi</p>")));
        QVERIFY(r.doc);
        QVERIFY(r.wanted.isEmpty());
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
        // only the dark text (#222) turns red: white fills, the blue button and grey text stay
        r.doc->setColorFilter([](const QColor &c) { return c.lightness() < 100 ? QColor(Qt::red) : c; });
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

    // litehtml never breaks inside a word: a long one ran out of the letter on the right
    void long_url_wraps_inside_the_width()
    {
        const QString url = QStringLiteral(
            "https://docs.example.org/v22/ru/ngfw/settings/users/ideco-client/ideco-client-macos#install-the-client");
        RenderJob j = job(QStringLiteral("<p>See <a href=\"%1\">%1</a> for details</p>").arg(url));
        j.width = 300;
        const Rendered r = run(j);
        QVERIFY(r.doc);
        QVERIFY2(r.size.width() <= j.width, qPrintable(QString::number(r.size.width())));
        bool found = false;
        QVector<QRect> pieces;
        r.doc->findText(url, {}, false, nullptr, &found, nullptr, &pieces);
        QVERIFY(found);
        QCOMPARE(r.doc->selectedText(), url);   // nothing inserted where it breaks
        QVERIFY(pieces.last().top() > pieces.first().bottom());   // on more than one line
        QCOMPARE(r.links.size(), 1);
        QCOMPARE(r.links.at(0).url, QUrl(url));
        for (const QRect &piece : std::as_const(pieces))
            QVERIFY(r.links.at(0).rect.contains(piece.center()));
    }
    void long_token_wraps_in_plain_text()
    {
        const QString token = QStringLiteral("QmFzZTY0").repeated(20);   // 160 characters, no URL-like breaks
        RenderJob j = job(renderSource({{"text", QStringLiteral("key: %1 end").arg(token)}}, {}, QColor()));
        j.width = 300;
        const Rendered r = run(j);
        QVERIFY(r.doc);
        QVERIFY2(r.size.width() <= j.width, qPrintable(QString::number(r.size.width())));
        bool found = false;
        r.doc->findText(token, {}, false, nullptr, &found, nullptr, nullptr);
        QVERIFY(found);
        QCOMPARE(r.doc->selectedText(), token);
    }
    void ordinary_words_stay_whole()
    {
        // only words too long for a line break inside: these move to the next line whole
        const QString word = QStringLiteral("высокопроизводительный-сервер");   // 29 characters
        RenderJob j = job(QStringLiteral("<p>%1 %1 %1 %1</p>").arg(word));
        j.width = 300;
        const Rendered r = run(j);
        QVERIFY(r.doc);
        bool found = false;
        QVector<QRect> pieces;
        r.doc->findText(word.chopped(1), {}, false, nullptr, &found, nullptr, &pieces);   // ends inside the word
        QVERIFY(found);
        QCOMPARE(pieces.size(), 1);   // one piece of text
    }

    void blank_letters_known()
    {
        // nothing to read or see: the window says so instead of an empty letter
        const struct
        {
            const char *html;
            bool blank;
        } cases[] = {
            {"<pre style=\"white-space:pre-wrap\"> \n\t </pre>", true},
            {"<html><head><title>News</title><style>p{color:red}</style></head>"
             "<body><p>&nbsp;&zwnj;&#8203;</p><script>go()</script></body></html>", true},
            {"<p><img src=\"cid:logo\"></p>", false},
            // a picture without text is something to see, even where litehtml can't draw it
            {"<svg width=\"10\" height=\"10\"><rect width=\"10\" height=\"10\"/></svg>", false},
            {"<video src=\"cid:clip\"></video>", false},
            {"<input type=\"IMAGE\" src=\"cid:logo\">", false},
            {"<table><tr><td background=\"cid:banner\" width=\"600\" height=\"300\">&nbsp;</td></tr></table>", false},
            {"<table bgcolor=\"#ff0000\"><tr><td>&nbsp;</td></tr></table>", false},
            {"<div style=\"height:300px;background-image:URL('cid:banner')\"></div>", false},
            {"<div style=\"color:red\"> </div>", true},
            {"<p> Hi </p>", false},
        };
        for (const auto &c : cases) {
            const Rendered r = run(job(QString::fromUtf8(c.html)));
            QVERIFY(r.doc);
            QVERIFY2(r.blank == c.blank, c.html);
        }
    }

    void garbage_html_renders()
    {
        const Rendered nested = run(job(QStringLiteral("<table><tr><td><div><b><i>").repeated(5000)), 30000);
        QVERIFY(nested.doc);
        const Rendered word = run(job(QString(1 << 20, QLatin1Char('x'))), 30000);
        QVERIFY(word.doc);
        // its first 10 000 characters wrap; the rest is one piece: 100 000 pieces took 8 s to lay out
        QVERIFY2(word.size.height() > 20 * 16 && word.size.height() < 20000, qPrintable(QString::number(word.size.height())));
    }
};

QTEST_MAIN(TstRenderer)
#include "tst_renderer.moc"
