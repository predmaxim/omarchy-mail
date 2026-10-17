#include "Compose.h"
#include <QTest>

using namespace om;

class TstMailto : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void simple_address()
    {
        const MailtoDraft d = parseMailto("mailto:anna@example.com");
        QCOMPARE(d.to, QString("anna@example.com"));
        QVERIFY(d.subject.isEmpty() && d.body.isEmpty());
    }
    void params_and_percent()
    {
        MailtoDraft d = parseMailto("mailto:a@x.org?cc=c@x.org&subject=Hi%20there%26more&body=line1%0Aline2%25");
        QCOMPARE(d.to, QString("a@x.org"));
        QCOMPARE(d.subject, QString("Hi there&more"));
        QCOMPARE(d.body, QString("line1\nline2%"));
        d = parseMailto("mailto:a@x.org?to=b@x.org");   // RFC 6068: `to` adds addresses
        QCOMPARE(d.to, QString("a@x.org, b@x.org"));
        d = parseMailto("mailto:?to=b@x.org&subject=S");
        QCOMPARE(d.to, QString("b@x.org"));
    }
    void cyrillic_subject()
    {
        const MailtoDraft d = parseMailto("mailto:a@x.org?subject=%D0%9F%D1%80%D0%B8%D0%B2%D0%B5%D1%82&body=Привет");
        QCOMPARE(d.subject, QString::fromUtf8("Привет"));
        QCOMPARE(d.body, QString::fromUtf8("Привет"));
    }
    void garbage_is_empty()
    {
        for (const char *g : {"", "garbage", "http://x.org/?subject=a", "mailto:", "mailto:?cc=a@b.c"}) {
            const MailtoDraft d = parseMailto(g);
            QVERIFY2(d.to.isEmpty() && d.subject.isEmpty() && d.body.isEmpty(), g);
        }
    }
};

QTEST_APPLESS_MAIN(TstMailto)
#include "tst_mailto.moc"
