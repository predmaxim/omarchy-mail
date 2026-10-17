#include "Format.h"
#include "I18n.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace om;

static qint64 at(int y, int mo, int d, int h = 12, int mi = 0)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi)).toSecsSinceEpoch();
}

static QJsonObject acc(const char *email, const QJsonValue &status, int queued = 0)
{
    return {{"email", email}, {"status", status}, {"queued", queued}};
}

class TstFormat : public QObject {
    Q_OBJECT
    const QDateTime now{QDate(2026, 10, 17), QTime(15, 0)};
    const QLocale ru{QStringLiteral("ru_RU")};
private Q_SLOTS:
    void init() { I18n::setRu(false); }
    void listDate()
    {
        QCOMPARE(format::listDate(at(2026, 10, 17, 10, 42), now, ru), QStringLiteral("10:42"));
        QCOMPARE(format::listDate(at(2026, 10, 16, 23, 10), now, ru), QStringLiteral("yesterday"));
        QCOMPARE(format::listDate(at(2026, 10, 13), now, ru), ru.dayName(2, QLocale::ShortFormat));
        QCOMPARE(format::listDate(at(2026, 9, 12), now, ru), ru.toString(QDate(2026, 9, 12), "d MMM"));
        QCOMPARE(format::listDate(at(2025, 12, 31), now, ru), ru.toString(QDate(2025, 12, 31), QLocale::ShortFormat));
        QCOMPARE(format::listDate(0, now, ru), QString());
    }
    void letterDate()
    {
        QCOMPARE(format::letterDate(at(2026, 10, 17, 10, 42), now, ru), QStringLiteral("today 10:42"));
        const qint64 u = at(2026, 9, 12, 8, 5);
        QCOMPARE(format::letterDate(u, now, ru), ru.toString(QDateTime::fromSecsSinceEpoch(u), "d MMM yyyy, HH:mm"));
        QCOMPARE(format::quoteDate(u, ru), ru.toString(QDateTime::fromSecsSinceEpoch(u), "d MMM yyyy, HH:mm"));
    }
    void size()
    {
        QCOMPARE(format::size(480), QStringLiteral("480 B"));
        QCOMPARE(format::size(245760), QStringLiteral("240 KB"));
        QCOMPARE(format::size(1258291), QStringLiteral("1.2 MB"));
        I18n::setRu(true);
        QCOMPARE(format::size(480), QStringLiteral("480 Б"));
        QCOMPARE(format::size(245760), QStringLiteral("240 КБ"));
        QCOMPARE(format::size(1258291), QStringLiteral("1,2 МБ"));
    }
    void folderLabel()
    {
        QCOMPARE(format::folderLabel("inbox", "INBOX"), QStringLiteral("Inbox"));
        QCOMPARE(format::folderLabel("sent", "Отправленные"), QStringLiteral("Sent"));
        QCOMPARE(format::folderLabel("other", "Работа"), QStringLiteral("Работа"));
    }
    void statusLine()
    {
        auto s = format::statusLine(false, {});
        QCOMPARE(s.text, QStringLiteral("Mail service is not running"));
        QVERIFY(s.error);

        const QJsonObject off{{"offline", "down"}};
        s = format::statusLine(true, {acc("a@x", off, 3)});
        QCOMPARE(s.text, QStringLiteral("offline · 3 queued"));
        QVERIFY(!s.error);
        I18n::setRu(true);
        QCOMPARE(format::statusLine(true, {acc("a@x", off, 3)}).text, QStringLiteral("нет сети · 3 в очереди"));
        I18n::setRu(false);

        s = format::statusLine(true, {acc("a@x", "online"), acc("b@x", off)});
        QCOMPARE(s.text, QStringLiteral("b@x offline"));
        s = format::statusLine(true, {acc("a@x", "auth_failed"), acc("b@x", "online")});
        QCOMPARE(s.text, QStringLiteral("a@x: login rejected"));
        QVERIFY(s.error);
        s = format::statusLine(true, {acc("a@x", "online", 2)});
        QCOMPARE(s.text, QStringLiteral("2 queued"));
        QCOMPARE(format::statusLine(true, {acc("a@x", "online")}).text, QString());
        QCOMPARE(format::statusLine(true, {acc("a@x", "connecting")}).text, QString());
        QCOMPARE(format::statusLine(true, {acc("a@x", off), acc("b@x", off), acc("c@x", "online")}).text, QStringLiteral("2 mailboxes offline"));
    }
};

QTEST_MAIN(TstFormat)
#include "tst_format.moc"
