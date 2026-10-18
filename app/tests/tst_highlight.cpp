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
    void html_word_right_after_a_tag()
    {
        QCOMPARE(highlight::html(QStringLiteral("<td>Счёт</td><td>Ozon</td>"), {"ozon"}, mark),
                 "<td>Счёт</td><td>" + open + "Ozon</span></td>");
    }
    void html_case_insensitive()
    {
        QCOMPARE(highlight::html(QStringLiteral("ДОГОВОР"), {"догов"}, mark), open + "ДОГОВ</span>ОР");
    }
    void links_in_text()
    {
        const QString r = highlight::links(QStringLiteral(
            "<p>Пишите на ivan@x.ru или смотрите https://x.ru/a?b=1. Также www.site.ru, (см. https://ru.wikipedia.org/wiki/X_(y))</p>"));
        QCOMPARE(r, QStringLiteral(
            "<p>Пишите на <a href=\"mailto:ivan@x.ru\">ivan@x.ru</a> или смотрите <a href=\"https://x.ru/a?b=1\">https://x.ru/a?b=1</a>."
            " Также <a href=\"https://www.site.ru\">www.site.ru</a>, (см. <a href=\"https://ru.wikipedia.org/wiki/X_(y)\">"
            "https://ru.wikipedia.org/wiki/X_(y)</a>)</p>"));
    }
    void links_skip_existing_and_raw()
    {
        const QString s = "<a href=\"https://a.ru\">https://a.ru</a><style>b@c.de</style><img alt=\"x@y.ru\"><!-- z@w.ru -->";
        QCOMPARE(highlight::links(s), s);
    }
    void links_url_wins_over_email()
    {
        QCOMPARE(highlight::links(QStringLiteral("https://user@host.ru/x")),
                 QStringLiteral("<a href=\"https://user@host.ru/x\">https://user@host.ru/x</a>"));
    }
    void links_keep_entities()
    {
        QCOMPARE(highlight::links(QStringLiteral("<p>https://x.ru/?a=1&amp;b=2</p>")),
                 QStringLiteral("<p><a href=\"https://x.ru/?a=1&amp;b=2\">https://x.ru/?a=1&amp;b=2</a></p>"));
        QCOMPARE(highlight::links(QStringLiteral("&lt;https://x.ru&gt;")),
                 QStringLiteral("&lt;<a href=\"https://x.ru\">https://x.ru</a>&gt;"));
    }
    void links_stop_at_nbsp_entity()
    {
        for (const char *nb : {"&nbsp;", "&#160;", "&#xa0;"})
            QCOMPARE(highlight::links(QString("<p>https://x.ru/a%1дальше</p>").arg(nb)),
                     QString("<p><a href=\"https://x.ru/a\">https://x.ru/a</a>%1дальше</p>").arg(nb));
    }
    void links_cyrillic_domain()
    {
        QCOMPARE(highlight::links(QStringLiteral("info@почта.рф")),
                 QStringLiteral("<a href=\"mailto:info@почта.рф\">info@почта.рф</a>"));
    }
    void links_then_search_highlight()
    {
        const QString r = highlight::html(highlight::links(QStringLiteral("info@почта.рф")), {"почта"}, mark);
        QCOMPARE(r, "<a href=\"mailto:info@почта.рф\">info@" + open + "почта</span>.рф</a>");
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
