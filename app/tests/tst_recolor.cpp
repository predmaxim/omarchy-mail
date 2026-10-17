#include "Recolor.h"

#include <QImage>
#include <QPainter>
#include <QTest>

using namespace om;

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
