#include "Recolor.h"

#include <QImage>
#include <QPainter>
#include <QTest>

#include <cmath>

using namespace om;

// WCAG 2 relative luminance and contrast ratio, apart from Recolor's own
static double lum(const QColor &c)
{
    const auto l = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * l(c.redF()) + 0.7152 * l(c.greenF()) + 0.0722 * l(c.blueF());
}
static double contrast(const QColor &a, const QColor &b)
{
    const double x = lum(a) + .05, y = lum(b) + .05;
    return qMax(x, y) / qMin(x, y);
}

class TstRecolor : public QObject
{
    Q_OBJECT
    const QColor bg{"#101318"}, fg{"#eaeff5"};

private Q_SLOTS:
    void light_white_becomes_bg() { QCOMPARE(recolor::map(Qt::white, bg, fg, false).rgb(), bg.rgb()); }
    void light_black_becomes_fg() { QCOMPARE(recolor::map(Qt::black, bg, fg, false).rgb(), fg.rgb()); }
    void dark_black_becomes_bg() { QCOMPARE(recolor::map(Qt::black, bg, fg, true).rgb(), bg.rgb()); }
    void dark_white_becomes_fg() { QCOMPARE(recolor::map(Qt::white, bg, fg, true).rgb(), fg.rgb()); }
    void hue_kept()
    {
        const QColor c("#1a73e8"), o = recolor::map(c, bg, fg, false);
        double d = qAbs(o.hslHueF() * 360 - c.hslHueF() * 360);
        d = qMin(d, 360 - d);
        QVERIFY2(d <= 3, qPrintable(QString::number(d)));
        QVERIFY(o.hslSaturationF() > 0.3);
    }
    void alpha_kept() { QCOMPARE(recolor::map(QColor(255, 0, 0, 128), bg, fg, false).alpha(), 128); }
    void light_theme_identity()
    {
        const QColor w = Qt::white, b = Qt::black;
        QCOMPARE(recolor::map(w, w, b, false).rgb(), w.rgb());
        QCOMPARE(recolor::map(b, w, b, false).rgb(), b.rgb());
    }
    void filter_matches_map()
    {
        auto f = recolor::filter(bg, fg, true);
        QCOMPARE(f(Qt::white).rgb(), fg.rgb());
    }
    // a grey that map() turns into a theme colour between bg and fg: our own quote bar and quote text
    void unmap_inverts_map()
    {
        const QColor light[] = {QColor("#fafafa"), QColor("#2a2a2a")};
        for (const auto &[b, f] : {std::pair{bg, fg}, std::pair{light[0], light[1]}})
            for (const QColor &c : {QColor("#3a3f4c"), f.darker(140), QColor("#c8c8c8")})
                for (bool dark : {false, true}) {
                    const QColor o = recolor::map(recolor::unmap(c, b, f, dark), b, f, dark);
                    const float lo = qMin(b.lightnessF(), f.lightnessF()), hi = qMax(b.lightnessF(), f.lightnessF());
                    QVERIFY2(qAbs(o.lightnessF() - qBound(lo, c.lightnessF(), hi)) < 0.02,
                             qPrintable(c.name() + " " + o.name() + (dark ? " dark" : " light")));
                }
    }
    // a theme colour with a hue (the accent) comes back with its hue too
    void unmap_keeps_hue()
    {
        for (const QColor &c : {QColor("#8999aa"), QColor("#e0a568"), QColor("#7aa2f7")})
            for (bool dark : {false, true}) {
                const QColor o = recolor::map(recolor::unmap(c, bg, fg, dark), bg, fg, dark);
                const int d = qAbs(o.red() - c.red()) + qAbs(o.green() - c.green()) + qAbs(o.blue() - c.blue());
                QVERIFY2(d < 30, qPrintable(c.name() + " " + o.name() + (dark ? " dark" : " light")));
            }
    }
    // every contrast becomes c^k, k = the theme's range in log space: saturated pairs no longer collapse
    void contrast_shrinks_by_the_theme_range()
    {
        const double k = std::log(contrast(bg, fg)) / std::log(21.0);
        const char *pairs[][2] = {{"#ffffff", "#6366f1"}, {"#0000ff", "#ffffff"}, {"#4674ca", "#f4f5f6"},
                                  {"#ffffff", "#6c5fc7"}, {"#8295b9", "#041866"}, {"#999999", "#ffffff"}};
        for (const auto &[a, b] : pairs)
            for (bool dark : {false, true}) {
                const double c0 = contrast(QColor(a), QColor(b));
                const double c1 = contrast(recolor::map(QColor(a), bg, fg, dark), recolor::map(QColor(b), bg, fg, dark));
                QVERIFY2(qAbs(c1 / std::pow(c0, k) - 1) < 0.01,
                         qPrintable(QString("%1/%2 %3 vs %4").arg(a, b).arg(c1).arg(std::pow(c0, k))));
            }
    }
    // the investigation's numbers for the user's theme black-sand: #00f links 1.92:1 -> 5.84:1,
    // the indigo header 1.41:1 -> 3.41:1
    void luminance_not_lightness()
    {
        const QColor b("#14181e"), f("#ced6e0"), blue = recolor::map(QColor("#0000ff"), b, f, false);
        QVERIFY2(qAbs(blue.red() - 0x86) + qAbs(blue.green() - 0x86) + qAbs(blue.blue() - 0xff) <= 6,
                 qPrintable(blue.name()));
        QVERIFY(qAbs(contrast(blue, b) - 5.84) < 0.03);
        const QColor header = recolor::map(Qt::white, b, f, false), title = recolor::map(QColor("#6366f1"), b, f, false);
        QVERIFY(qAbs(contrast(header, title) - 3.41) < 0.03);
    }
    // our own colours (quote greys, the accent) come back exactly from 8-bit colours, as litehtml keeps them
    void unmap_is_exact()
    {
        const QColor sand[] = {QColor("#14181e"), QColor("#ced6e0")}, light[] = {QColor("#fafafa"), QColor("#2a2a2a")};
        for (auto [b, f] : {std::pair{bg, fg}, std::pair{sand[0], sand[1]}, std::pair{light[0], light[1]}})
            for (const char *t :
                 {"#3a3f4c", "#5c6370", "#8999aa", "#7aa2f7", "#e0a568", "#ced6e0", "#525d6a", "#c8c8c8", "#9399a0"})
                for (bool dark : {false, true}) {
                    const QColor c(t);
                    if (lum(c) < qMin(lum(b), lum(f)) || lum(c) > qMax(lum(b), lum(f)))
                        continue;   // outside the theme's range
                    const QColor u = recolor::unmap(c, b, f, dark), o = recolor::map(u, b, f, dark);
                    QCOMPARE(u, QColor(u.name()));
                    QVERIFY2(qAbs(o.red() - c.red()) + qAbs(o.green() - c.green()) + qAbs(o.blue() - c.blue()) <= 3,
                             qPrintable(c.name() + " " + o.name()));
                }
        // past the range: its nearest end
        QCOMPARE(recolor::map(recolor::unmap(QColor("#1e1e1e"), light[0], light[1], false), light[0], light[1], false).name(),
                 QString("#2a2a2a"));

        // the stock Omarchy themes' accents (background, foreground, accent): no byte a step away maps nearer, and
        // map() lands within 3, but hackerman's in a light letter 7 off, the limit unmap's shortcut names
        const char *stock[][3] = {
            {"#1e1e2e", "#cdd6f4", "#89b4fa"}, {"#eff1f5", "#4c4f69", "#1e66f5"}, {"#060B1E", "#ffcead", "#7d82d9"},
            {"#2d353b", "#d3c6aa", "#7fbbb3"}, {"#FFFCF0", "#100F0F", "#205EA6"}, {"#282828", "#d4be98", "#7daea3"},
            {"#0B0C16", "#ddf7ff", "#82FB9C"}, {"#142620", "#dfede5", "#8bcbb1"}, {"#1f1f28", "#dcd7ba", "#dcd7ba"},
            {"#0c0b0c", "#FAFCFB", "#b59790"}, {"#16242d", "#d6e2ee", "#8bc9eb"}, {"#fafafa", "#212121", "#3264eb"},
            {"#121212", "#bebebe", "#e68e0d"}, {"#222222", "#c2c2b0", "#78824b"}, {"#2e3440", "#d8dee9", "#81a1c1"},
            {"#05182e", "#f6dcac", "#faa968"}, {"#2c2525", "#e6d9db", "#f38d70"}, {"#faf4ed", "#575279", "#56949f"},
            {"#101315", "#cacccc", "#798186"}, {"#1c1c1e", "#f5f5f7", "#ff9f0a"}, {"#1a1b26", "#a9b1d6", "#7aa2f7"},
            {"#000000", "#ffffff", "#8d8d8d"}, {"#ffffff", "#000000", "#6e6e6e"}};
        const auto off = [](const QColor &a, const QColor &b) {
            return std::abs(a.redF() - b.redF()) + std::abs(a.greenF() - b.greenF()) + std::abs(a.blueF() - b.blueF());
        };
        for (const auto &[sb, sf, sa] : stock)
            for (bool dark : {false, true}) {
                const QColor b(sb), f(sf), c(sa), u = recolor::unmap(c, b, f, dark), o = recolor::map(u, b, f, dark);
                const QString what = c.name() + " " + o.name() + (dark ? " dark" : " light");
                QCOMPARE(u, QColor(u.name()));
                QVERIFY2(qAbs(o.red() - c.red()) + qAbs(o.green() - c.green()) + qAbs(o.blue() - c.blue())
                             <= (c == QColor("#82FB9C") && !dark ? 7 : 3),
                         qPrintable(what));
                for (int dr = -1; dr <= 1; ++dr)
                    for (int dg = -1; dg <= 1; ++dg)
                        for (int db = -1; db <= 1; ++db) {
                            const QColor n(qBound(0, u.red() + dr, 255), qBound(0, u.green() + dg, 255),
                                           qBound(0, u.blue() + db, 255));
                            QVERIFY2(off(recolor::map(n, b, f, dark), c) >= off(o, c), qPrintable(what + " " + n.name()));
                        }
            }
    }
    void dark_detection()
    {
        QImage a(200, 200, QImage::Format_RGB32);
        a.fill(Qt::white);
        QPainter p(&a);
        for (int y = 0; y < 200; y += 10)
            p.fillRect(0, y, 200, 2, Qt::black);
        p.end();
        QVERIFY(!recolor::isDarkLetter(a));

        QImage b(200, 200, QImage::Format_RGB32);
        b.fill(QColor("#111111"));
        QPainter q(&b);
        for (int y = 0; y < 200; y += 10)
            q.fillRect(0, y, 200, 2, Qt::white);
        q.end();
        QVERIFY(recolor::isDarkLetter(b));

        QImage c(200, 200, QImage::Format_RGB32);
        c.fill(Qt::white);
        QPainter r(&c);
        r.fillRect(0, 120, 200, 80, Qt::black);
        r.end();
        QVERIFY(!recolor::isDarkLetter(c));
    }
};

QTEST_MAIN(TstRecolor)
#include "tst_recolor.moc"
