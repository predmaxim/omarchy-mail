#include "Highlight.h"

#include <QTest>

using namespace om;

class TstHighlight : public QObject
{
    Q_OBJECT
    const QColor mark{255, 200, 0};
    const QString open = QStringLiteral("<span style=\"background-color:rgba(255,200,0,0.4)\">");

private Q_SLOTS:
    void words_drop_symbols()
    {
        QCOMPARE(highlight::words(QStringLiteral("Договор № 17 - ромашка")),
                 (QStringList{"договор", "17", "ромашка"}));
    }
    void html_skips_tags()
    {
        const QString r = highlight::html(QStringLiteral("<a href=\"/договор\">Договору</a>"), {"догов"}, mark);
        QCOMPARE(r, "<a href=\"/договор\">" + open + "Догов</span>ору</a>");
    }
    void html_word_start_only()
    {
        QCOMPARE(highlight::html(QStringLiteral("переговоры говорить"), {"гов"}, mark),
                 "переговоры " + open + "гов</span>орить");
    }
    void html_skips_style_and_entities()
    {
        QCOMPARE(highlight::html(QStringLiteral("<style>.amp{}</style>AT&amp;T amp"), {"amp"}, mark),
                 "<style>.amp{}</style>AT&amp;T " + open + "amp</span>");
    }
    void html_skips_comment_script_title()
    {
        const QString s = "<!-- amp --><title>amp</title><script>var amp;</script>";
        QCOMPARE(highlight::html(s, {"amp"}, mark), s);
    }
    void html_case_insensitive()
    {
        QCOMPARE(highlight::html(QStringLiteral("ДОГОВОР"), {"догов"}, mark), open + "ДОГОВ</span>ОР");
    }
    void styled_escapes()
    {
        const QString r = highlight::styled(QStringLiteral("<b>x</b> договор"), {"догов"}, mark);
        QVERIFY(r.contains("&lt;b&gt;"));
        QVERIFY(r.contains("<b><font color=\"#"));
        QVERIFY(!r.contains("<b>x"));
    }
};

QTEST_MAIN(TstHighlight)
#include "tst_highlight.moc"
