#include "Compose.h"
#include "Format.h"
#include "I18n.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace om;
using Pairs = QList<QPair<QString, QString>>;

static QJsonObject full(qint64 id, const QString &text = "Привет!\n\nАнна")
{
    const qint64 ts = QDateTime(QDate(2026, 10, 17), QTime(10, 42)).toSecsSinceEpoch();
    return {{"summary", QJsonObject{{"id", id}, {"account", "me@gmail.com"}, {"subject", "счёт"}, {"date", ts},
                                    {"from", QJsonObject{{"name", "Анна Смирнова"}, {"addr", "anna@example.com"}}}}},
            {"text", text}};
}

class TstCompose : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
private Q_SLOTS:
    void init() { I18n::setRu(false); }
    void parse_addresses()
    {
        QString bad;
        Pairs r = Compose::parseAddresses("\"Анна Смирнова\" <anna@example.com>, ivan@example.org", &bad);
        QCOMPARE(r, (Pairs{{"Анна Смирнова", "anna@example.com"}, {"", "ivan@example.org"}}));
        QVERIFY(bad.isEmpty());
        Compose::parseAddresses("ann", &bad);
        QCOMPARE(bad, QString("ann"));
        bad.clear();
        QCOMPARE(Compose::parseAddresses("a@b.c; c@d.e", &bad).size(), 2);
        r = Compose::parseAddresses("\"Smith, Bob\" <b@x.org>", &bad);
        QCOMPARE(r.size(), 1);
    }
    void reply_subject()
    {
        QCOMPARE(Compose::replySubject("счёт"), QString("Re: счёт"));
        QCOMPARE(Compose::replySubject("RE: счёт"), QString("RE: счёт"));
        QCOMPARE(Compose::replySubject(" re:x"), QString(" re:x"));
    }
    void quote_format()
    {
        I18n::setRu(true);
        const QLocale ru(QLocale::Russian, QLocale::Russia);
        const qint64 ts = full(1).value("summary").toObject().value("date").toInteger();
        QCOMPARE(Compose::quote(full(1), ru),
                 format::quoteDate(ts, ru) + ", Анна Смирнова пишет:\n> Привет!\n>\n> Анна");
    }
    void reply_prefills()
    {
        Compose c(dir.filePath("prefill"));
        c.startReply(full(41));
        QVERIFY(c.isOpen() && c.isReply());
        QCOMPARE(c.account(), QString("me@gmail.com"));
        QCOMPARE(c.to(), QString("Анна Смирнова <anna@example.com>"));
        QCOMPARE(c.subject(), QString("Re: счёт"));
        QCOMPARE(c.bodyCursor(), 0);
        QVERIFY(c.body().startsWith("\n\n"));
    }
    void drafts_survive_close()
    {
        const QString d = dir.filePath("survive");
        {
            Compose c(d);
            c.startNew("me@gmail.com");
            c.setTo("a@x.org");
            c.setSubject("тема");
            c.setBody("текст");
            c.close();
            QVERIFY(!c.isOpen());
            c.startReply(full(41));
            c.setBody("ответ 41");
            c.close();
            c.startReply(full(42));
            QVERIFY(c.body().startsWith("\n\n"));
            c.close();
        }
        Compose c(d);
        c.startNew("other");
        QCOMPARE(c.to(), QString("a@x.org"));
        QCOMPARE(c.subject(), QString("тема"));
        QCOMPARE(c.body(), QString("текст"));
        c.startReply(full(41));
        QCOMPARE(c.body(), QString("ответ 41"));
        c.close();
        QCOMPARE(QFileInfo(d + "/new.json").permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::Permissions());
    }
    void autosave_after_silence()
    {
        const QString d = dir.filePath("auto");
        Compose c(d);
        c.startNew("a");
        c.setTo("x@y.zz");
        QVERIFY(!QFile::exists(d + "/new.json"));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(d + "/new.json"), 3000);
    }
    void discard_needs_second_press()
    {
        const QString d = dir.filePath("discard");
        Compose c(d);
        c.startNew("a");
        c.setBody("x");
        c.close();
        c.startNew("a");
        c.discard();
        QVERIFY(!c.note().isEmpty());
        QVERIFY(c.isOpen());
        QVERIFY(QFile::exists(d + "/new.json"));
        c.discard();
        QVERIFY(!QFile::exists(d + "/new.json"));
        QVERIFY(!c.isOpen());
    }
    void paste_image_becomes_png()
    {
        Compose c(dir.filePath("paste"));
        c.startNew("a");
        QGuiApplication::clipboard()->setImage(QImage(4, 4, QImage::Format_RGB32));
        QVERIFY(c.paste());
        QCOMPARE(c.attachments().size(), 1);
        const QString n1 = c.attachments().at(0).toMap().value("name").toString();
        QVERIFY2(n1.startsWith("screenshot-") && n1.endsWith(".png"), qPrintable(n1));
        QVERIFY(QFile::exists(dir.filePath("paste/files/" + n1)));
        QVERIFY(c.paste());
        const QString n2 = c.attachments().at(1).toMap().value("name").toString();
        QVERIFY(n2 != n1);
        if (n2.endsWith("-2.png"))   // same minute (minute rollover between pastes is possible)
            QVERIFY(n2.startsWith(n1.left(n1.size() - 4)));
        c.removeAttachment(0);
        QVERIFY(!QFile::exists(dir.filePath("paste/files/" + n1)));
        QGuiApplication::clipboard()->setText("x");
        QVERIFY(!c.paste());
    }
    void paste_files_and_add_files()
    {
        Compose c(dir.filePath("files"));
        c.startNew("a");
        QFile f(dir.filePath("doc.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hi");
        f.close();
        c.addFiles({QUrl::fromLocalFile(f.fileName()), QUrl::fromLocalFile(dir.filePath("missing")),
                    QUrl("https://x.org/a")});
        QCOMPARE(c.attachments().size(), 1);
        QCOMPARE(c.attachments().at(0).toMap().value("name").toString(), QString("doc.txt"));
    }
    void request_shape()
    {
        Compose c(dir.filePath("req"));
        c.startReply(full(41));
        QFile f(dir.filePath("a.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        c.addFiles({QUrl::fromLocalFile(f.fileName())});
        QString err;
        QJsonObject r = c.request(&err);
        QStringList keys = r.keys();
        keys.sort();
        QCOMPARE(keys, (QStringList{"account", "attachments", "cmd", "reply_to", "subject", "text", "to"}));
        QCOMPARE(r.value("reply_to").toInteger(), 41);
        QCOMPARE(r.value("attachments").toArray().at(0).toString(), f.fileName());
        QCOMPARE(r.value("to").toArray().at(0).toObject().value("addr").toString(), QString("anna@example.com"));
        c.startNew("a");
        c.setTo("");
        r = c.request(&err);
        QCOMPARE(err, QString("Add a recipient"));
        c.setTo("bad");
        c.request(&err);
        QCOMPARE(err, QString("Invalid address: bad"));
        c.setTo("a@b.cc");
        QVERIFY(!c.request(&err).contains("reply_to"));
    }
    void sent_cleans_up()
    {
        const QString d = dir.filePath("sent");
        Compose c(d);
        c.startNew("a");
        QGuiApplication::clipboard()->setImage(QImage(4, 4, QImage::Format_RGB32));
        QVERIFY(c.paste());
        c.close();
        const QString png = QDir(d + "/files").entryInfoList(QDir::Files).at(0).absoluteFilePath();
        c.startNew("a");
        QCOMPARE(c.attachments().size(), 1);
        c.sent();
        QVERIFY(!QFile::exists(png));
        QVERIFY(!QFile::exists(d + "/new.json"));
        QVERIFY(!c.isOpen());
    }
    void parse_mailto()
    {
        MailtoDraft m = parseMailto("mailto:b@y.org?subject=Hi%20there&body=a%0Ab");
        QCOMPARE(m.to, QString("b@y.org"));
        QCOMPARE(m.subject, QString("Hi there"));
        QCOMPARE(m.body, QString("a\nb"));
        m = parseMailto("mailto:x@y.org?subject=%D1%81%D1%87%D1%91%D1%82");
        QCOMPARE(m.subject, QString("счёт"));
    }
    void mailto_appends()
    {
        Compose c(dir.filePath("mailto"));
        c.startNew("a");
        c.setTo("a@x.org");
        c.close();
        c.startMailto(QUrl("mailto:b@y.org?subject=Hi&body=text"), "a");
        QVERIFY(c.to().contains("a@x.org") && c.to().contains("b@y.org"));
        QCOMPARE(c.subject(), QString("Hi"));
        QCOMPARE(c.body(), QString("text"));
        c.setSubject("kept");
        c.startMailto(QUrl("mailto:b@y.org?subject=Other&body=more"), "a");
        QCOMPARE(c.subject(), QString("kept"));
        QCOMPARE(c.body(), QString("text"));
        QCOMPARE(c.to().count("b@y.org"), 1);
    }
    void complete_to()
    {
        QCOMPARE(Compose::completeTo("a@x.org, ан", "Анна", "anna@example.com"),
                 QString("a@x.org, Анна <anna@example.com>, "));
        QCOMPARE(Compose::completeTo("", "Smith, Bob", "b@x.org"), QString("\"Smith, Bob\" <b@x.org>, "));
        Compose c(dir.filePath("pick"));
        c.startNew("a");
        c.setTo("ан");
        c.setSuggestions({QVariantMap{{"name", "Анна"}, {"addr", "anna@example.com"}}});
        c.pickSuggestion(0);
        QCOMPARE(c.to(), QString("Анна <anna@example.com>, "));
        QVERIFY(c.suggestions().isEmpty());
    }
};

QTEST_MAIN(TstCompose)
#include "tst_compose.moc"
