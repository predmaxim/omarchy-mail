#include "Renderer.h"
#include "Letter.h"
#include "Recolor.h"

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

static bool has(const Rendered &r, const QString &text)
{
    bool found = false;
    r.doc->findText(text, {}, false, nullptr, &found, nullptr, nullptr);
    return found;
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
        j.background = QColor(QStringLiteral("#101318"));
        j.foreground = QColor(QStringLiteral("#eaeff5"));
        j.muted = QColor(QStringLiteral("#3a3f4c"));
        j.dim = j.foreground.darker(140);
        j.accent = QColor(QStringLiteral("#8999aa"));
        j.quotesLabel = QStringLiteral("Show quoted text");
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

    // 1pt = 4/3 px, as in browsers: at qlitehtml's 11/12 px a payslip in 8pt was unreadable
    void points_are_css_points()
    {
        const auto height = [&](const QString &style) {
            const Rendered r = run(job(QStringLiteral("<body style=\"margin:0\"><div style=\"%1\">Hg</div></body>").arg(style)));
            return r.doc ? r.size.height() : -1;
        };
        QCOMPARE(height("font-size:12pt"), height("font-size:16px"));
        QCOMPARE(height("height:1in;overflow:hidden"), 96);
        QCOMPARE(height("height:2.54cm;overflow:hidden"), 96);
        QCOMPARE(height("height:6pc;overflow:hidden"), 96);
    }

    // letter text is never below 13 px (a minimum font size): a payslip in 8pt looked 2/3 the size of others
    void text_is_at_least_13px()
    {
        const auto line = [&](const QString &style) {
            const Rendered r = run(job(QStringLiteral("<body style=\"margin:0\"><a href=\"https://x.org/\" style=\"display:block;%1\">"
                                                      "Hg word<br>Hg word</a></body>").arg(style)));
            return r.links.size() == 1 ? r.links.at(0).rect.height() : -1;
        };
        const int small = line("font-size:8pt"), ref = line("font-size:13px");
        QVERIFY(ref > 0);
        QVERIFY2(small >= ref, qPrintable(QString("%1 < %2").arg(small).arg(ref)));
        QVERIFY(small >= 2 * 13);   // two lines, none squeezed below the font
        QCOMPARE(line("font-size:20px"), 2 * 34);   // larger text is untouched (line-height 1.7)
        const Rendered r = run(job(QStringLiteral("<div style=\"font-size:8pt\">Payslip total</div>")));
        QVERIFY(has(r, QStringLiteral("Payslip total")));
    }

    // A newsletter's 600 px shell: its rule for phones applies only in a narrow window, and a centred
    // cell or <center> centres it, as in browsers (HTML "align descendants", WebKit's -webkit-center)
    void fixed_width_shell_centred()
    {
        const QString link = QStringLiteral("<a href=\"https://x.org/\" style=\"display:block\">Banner</a>");
        const QString shell = QStringLiteral(
            "<style>@media only screen and (max-width:620px){.shell{width:100%!important;max-width:100%!important}}</style>"
            "<body style=\"margin:0\"><table width=\"100%\" cellpadding=\"0\" cellspacing=\"0\"><tr><td align=\"center\">"
            "<table class=\"shell\" width=\"600\" cellpadding=\"0\" cellspacing=\"0\" style=\"width:600px;max-width:600px\">"
            "<tr><td>%1</td></tr></table></td></tr></table></body>").arg(link);
        const auto box = [&](const QString &html, int width) {
            RenderJob j = job(html);
            j.width = width;
            const Rendered r = run(j);
            return r.links.size() == 1 ? r.links.at(0).rect : QRect();
        };
        QRect r = box(shell, 760);
        QCOMPARE(r.width(), 600);
        QCOMPARE(r.left(), 80);
        QCOMPARE(box(shell, 500).width(), 500);
        r = box(QStringLiteral("<body style=\"margin:0\"><center><table width=\"600\" cellpadding=\"0\" cellspacing=\"0\">"
                               "<tr><td>%1</td></tr></table></center></body>").arg(link), 760);
        QCOMPARE(r.width(), 600);
        QCOMPARE(r.left(), 80);
        // a picture block inside the centred cell too
        r = box(QStringLiteral("<body style=\"margin:0\"><div align=\"center\"><a href=\"https://x.org/\" style=\"display:block;"
                               "width:200px\">Banner</a></div></body>"), 760);
        QCOMPARE(r.left(), 280);
        // the table's cellpadding pads its cells (litehtml kept 1 px for "0" and none for "10")
        QCOMPARE(box(QStringLiteral("<body style=\"margin:0\"><table cellpadding=\"10\" cellspacing=\"0\"><tr><td>%1</td></tr>"
                                    "</table></body>").arg(link), 760).left(), 10);
        QCOMPARE(box(QStringLiteral("<body style=\"margin:0\"><a href=\"https://x.org/\" style=\"display:block;width:50vw\">"
                                    "Banner</a></body>"), 760).width(), 380);
    }

    // our user-agent sheet, like Geary's: roomier lines, quotes with a bar; the letter's own CSS wins
    void ua_sheet_like_geary()
    {
        const QString a = QStringLiteral("<a href=\"https://x.org/\" style=\"display:block\">Hg</a>");
        const auto box = [&](const QString &html) {
            const Rendered r = run(job(html));
            return r.links.size() == 1 ? r.links.at(0).rect : QRect();
        };
        QRect r = box(a);
        QCOMPARE(r.left(), 12);    // body margin
        QCOMPARE(r.height(), 27);  // line-height 1.7 of 16 px
        QCOMPARE(box("<h1>" + a + "</h1>").height(), 38);   // 1.2 of 32 px
        QCOMPARE(box("<div style=\"font-size:20px\"><table><tr><td>" + a + "</td></tr></table></div>").height(), 34);
        r = box("<blockquote>" + a + "</blockquote>");
        QCOMPARE(r.left(), 12 + 16 + 3 + 8);   // margin, bar, padding
        QCOMPARE(r.right() + 1, 760 - 12);     // nothing taken on the right
        QCOMPARE(box("<body style=\"margin:0;line-height:20px\">" + a + "</body>"), QRect(0, 0, 760, 20));
        RenderJob pre = job("<pre>" + QStringLiteral("word ").repeated(400) + "</pre>");
        const Rendered p = run(pre);
        QVERIFY(p.doc);
        QVERIFY2(p.size.width() <= 760, qPrintable(QString::number(p.size.width())));
    }

    // the roomy line-height must not open a gap under pictures in a newsletter's cells: a line with no
    // text is as tall as its pictures, as in browsers' quirks mode (no doctype, XHTML Transitional)
    void no_gap_under_pictures()
    {
        const auto height = [&](const QString &cell) {
            RenderJob j = job(QStringLiteral("<table cellpadding=\"0\" cellspacing=\"0\"><tr><td>%1</td></tr></table>").arg(cell));
            j.inlinePaths.insert(QStringLiteral("logo@x"), fixture("logo.png"));
            const Rendered r = run(j);
            return r.doc ? r.size.height() : -1;
        };
        const QString img = QStringLiteral("<img src=\"cid:logo@x\" width=\"300\" height=\"40\"%1>");
        const int block = height(img.arg(" style=\"display:block\""));
        QCOMPARE(height("<a href=\"https://x.org/\">" + img.arg("") + "</a>"), block);
        QCOMPARE(height(img.arg("") + " " + img.arg("")), block);
        QCOMPARE(height("<a href=\"https://x.org/\">" + img.arg("") + "</a>" + img.arg(" style=\"display:inline\"")), block);
        QVERIFY(height("Hg " + img.arg("")) > block);   // text keeps its line
    }

    // a line with only a line break is a blank line, also inside an inline element (Blink counts <br> as text)
    void blank_lines_stay()
    {
        const auto height = [&](const QString &html) {
            const Rendered r = run(job(html));
            return r.doc ? r.size.height() : -1;
        };
        const int two = height("<p>a<br><br>b</p>");
        QCOMPARE(height("<p>a<br><span><br></span>b</p>"), two);
        QCOMPARE(height("<p><font>a<br></font><font><br></font><font>b</font></p>"), two);
        QCOMPARE(height("<pre>a\n<span>\n</span>b</pre>"), height("<pre>a\n\nb</pre>"));
    }

    // the colour filter turns the quote's bar into the theme's muted and its text into dim, on a light
    // letter and on a dark one
    void quote_colours_are_the_theme_s()
    {
        const auto near = [](const QColor &a, const QColor &b) {
            return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue()) < 30;
        };
        for (const char *style : {"", "background:#000;color:#fff"}) {
            // (qlitehtml leaves the background of <body> and <html> to the view: a div carries it)
            const RenderJob j = job(QStringLiteral("<body style=\"margin:0\"><div style=\"%1\"><blockquote>"
                                                   "<div style=\"height:40px;background-color:currentColor\"></div>"
                                                   "</blockquote><div style=\"height:400px\"></div></div></body>").arg(style));
            const Rendered r = run(j);
            QVERIFY(r.doc);
            QCOMPARE(r.dark, *style != 0);
            QImage img(760, r.size.height(), QImage::Format_RGB32);
            img.fill(j.background);
            r.doc->setColorFilter(recolor::filter(j.background, j.foreground, r.dark));
            QPainter painter(&img);
            r.doc->draw(&painter, img.rect());
            painter.end();
            const QColor bar = img.pixelColor(16 + 1, 30), text = img.pixelColor(16 + 3 + 8 + 10, 30);
            QVERIFY2(near(bar, j.muted), qPrintable(bar.name() + " " + style));
            QVERIFY2(near(text, j.dim), qPrintable(text.name() + " " + style));
        }
    }

    // Like Geary (W37): a tall top-level quote is folded into a link, a short one stays; Gmail's
    // "On … wrote:" line stays above the link
    void tall_quotes_collapse()
    {
        QString lines;
        for (int i = 1; i <= 20; ++i)
            lines += QStringLiteral("Quoted line %1<br>").arg(i);
        RenderJob j = job(QStringLiteral(
            "<p>Thanks, see below.</p><div class=\"gmail_quote\"><div class=\"gmail_attr\">On Monday Ann wrote:<br></div>"
            "<blockquote>%1<blockquote>Inner<blockquote>Deepest</blockquote></blockquote></blockquote></div>"
            "<p>And the short one:</p><blockquote>Tiny quote</blockquote>").arg(lines));
        Rendered r = run(j);
        QVERIFY(r.doc);
        QVERIFY(!has(r, "Quoted line 7"));
        QVERIFY(!has(r, "Deepest"));
        QVERIFY(has(r, "On Monday Ann wrote:"));
        QVERIFY(has(r, "Tiny quote"));
        QVERIFY(has(r, "Show quoted text (3)"));   // three levels inside
        QCOMPARE(r.links.size(), 1);
        QCOMPARE(r.links.at(0).url, QUrl("omarchy-mail:quotes/0"));

        j.expanded = {0};
        r = run(j);
        QVERIFY(has(r, "Quoted line 7"));
        QVERIFY(has(r, "Deepest"));
        QVERIFY(r.links.isEmpty());
    }

    // the link is ours: the letter's CSS for a and div neither hides nor recolours it; it's the theme's accent
    void collapse_link_is_ours()
    {
        QString lines;
        for (int i = 1; i <= 20; ++i)
            lines += QStringLiteral("Quoted line %1<br>").arg(i);
        const RenderJob j = job(QStringLiteral(
            "<style>a{display:none!important;color:#000!important;visibility:hidden;font-size:0;text-decoration:none}"
            "div{display:none}</style><p>See below.</p><blockquote>%1</blockquote>").arg(lines));
        const Rendered r = run(j);
        QVERIFY(r.doc);
        QCOMPARE(r.links.size(), 1);
        const QRect link = r.links.at(0).rect;
        QVERIFY2(link.width() > 50 && link.height() > 10, qPrintable(QStringLiteral("%1x%2").arg(link.width()).arg(link.height())));
        QImage img(760, r.size.height(), QImage::Format_RGB32);
        img.fill(j.background);
        r.doc->setColorFilter(recolor::filter(j.background, j.foreground, r.dark));
        QPainter painter(&img);
        r.doc->draw(&painter, img.rect());
        painter.end();
        int best = 999;
        for (int y = link.top(); y <= link.bottom(); ++y)
            for (int x = link.left(); x <= link.right(); ++x) {
                const QColor p = img.pixelColor(x, y);
                best = qMin(best, qAbs(p.red() - j.accent.red()) + qAbs(p.green() - j.accent.green())
                                      + qAbs(p.blue() - j.accent.blue()));
            }
        QVERIFY2(best < 30, qPrintable(QString::number(best)));
    }

    // plain text: runs of "> " lines are quotes too, without the marks
    void plain_text_quotes_become_blockquotes()
    {
        QCOMPARE(renderSource({{"text", "Hi\n> a\n> > b\n>> c\n>\n> d\nBye\n"}}, {}, QColor()),
                 QStringLiteral("<pre style=\"white-space:pre-wrap;font-family:monospace\">Hi<blockquote>a<blockquote>b\nc"
                                "</blockquote>\nd</blockquote>Bye\n</pre>"));
        QString text = QStringLiteral("On Monday Ann wrote:\n");
        for (int i = 1; i <= 20; ++i)
            text += QStringLiteral("> Quoted line %1 <https://x.org/%1>\n").arg(i);
        const Rendered r = run(job(renderSource({{"text", text + "Bye"}}, {}, QColor())));
        QVERIFY(r.doc);
        QVERIFY(has(r, "On Monday Ann wrote:"));
        QVERIFY(!has(r, "Quoted line 7"));
        QVERIFY(has(r, "Show quoted text (1)"));
        QVERIFY(has(r, "Bye"));
        QCOMPARE(r.links.size(), 1);
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
