#include "I18n.h"
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QTest>

using namespace om;

static void env(const char *all, const char *msgs, const char *lang, const char *time = "")
{
    qputenv("LC_ALL", all);
    qputenv("LC_MESSAGES", msgs);
    qputenv("LANG", lang);
    qputenv("LC_TIME", time);
}

class TstI18n : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void language_from_env()
    {
        env("ru_RU.UTF-8", "", "");
        QVERIFY(I18n::ruFromEnv());
        env("", "ru_RU.UTF-8", "en_US.UTF-8");
        QVERIFY(I18n::ruFromEnv());
        env("", "", "en_US.UTF-8");
        QVERIFY(!I18n::ruFromEnv());
        env("C.UTF-8", "", "ru_RU.UTF-8");
        QVERIFY(!I18n::ruFromEnv());
    }
    void table_lookup()
    {
        I18n::setRu(true);
        QCOMPARE(t("Inbox"), QStringLiteral("Входящие"));
        QCOMPARE(t("Not in the table"), QStringLiteral("Not in the table"));
        I18n::setRu(false);
        QCOMPARE(t("Inbox"), QStringLiteral("Inbox"));
        QCOMPARE(I18n::loadTable("# c\nA\tB\n\nbad\n").value("A"), QStringLiteral("B"));
    }
    void plurals()
    {
        I18n::setRu(true);
        QCOMPARE(tn("%n queued", 3), QStringLiteral("3 в очереди"));
        const char *k = "%n mailbox offline|%n mailboxes offline";
        QCOMPARE(tn(k, 1), QStringLiteral("1 ящик не в сети"));
        QCOMPARE(tn(k, 3), QStringLiteral("3 ящика не в сети"));
        QCOMPARE(tn(k, 5), QStringLiteral("5 ящиков не в сети"));
        QCOMPARE(tn(k, 11), QStringLiteral("11 ящиков не в сети"));
        QCOMPARE(tn(k, 21), QStringLiteral("21 ящик не в сети"));
        I18n::setRu(false);
        QCOMPARE(tn(k, 1), QStringLiteral("1 mailbox offline"));
        QCOMPARE(tn(k, 2), QStringLiteral("2 mailboxes offline"));
    }
    void time_locale()
    {
        env("", "", "en_US.UTF-8", "ru_RU.UTF-8");
        QCOMPARE(timeLocale().name(), QStringLiteral("ru_RU"));
    }
    void every_string_translated()
    {
        QFile f(QStringLiteral(OM_SOURCE_DIR "/i18n/ru.tsv"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const auto table = I18n::loadTable(f.readAll());
        QStringList missing;
        const auto scan = [&](const QString &glob, const QString &re) {
            QDirIterator it(QStringLiteral(OM_SOURCE_DIR), {glob}, QDir::Files, QDirIterator::Subdirectories);
            const QRegularExpression rx(re);
            while (it.hasNext()) {
                const QString path = it.next();
                if (!path.contains(glob.startsWith("*.cpp") ? "/src/" : "/qml/"))
                    continue;
                QFile s(path);
                QVERIFY(s.open(QIODevice::ReadOnly));
                auto m = rx.globalMatch(QString::fromUtf8(s.readAll()));
                while (m.hasNext()) {
                    QString lit = m.next().captured(1);
                    lit.replace(QStringLiteral("\\\""), QStringLiteral("\""));
                    if (!table.contains(lit))
                        missing << lit;
                }
            }
        };
        scan(QStringLiteral("*.cpp"), QStringLiteral("\\b(?:om::)?tn?\\(\"((?:[^\"\\\\]|\\\\.)*)\""));
        if (QDir(QStringLiteral(OM_SOURCE_DIR "/qml")).exists())
            scan(QStringLiteral("*.qml"), QStringLiteral("i18n\\.tn?\\(\"((?:[^\"\\\\]|\\\\.)*)\""));
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QStringLiteral(" | "))));
    }
};

QTEST_MAIN(TstI18n)
#include "tst_i18n.moc"
