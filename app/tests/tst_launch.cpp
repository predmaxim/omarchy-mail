#include "LaunchStub.h"
#include "Launch.h"
#include "Letter.h"
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace om;
using namespace om::test;

static const char kCsp[] = "<!doctype html><meta charset=\"utf-8\">"
                           "<meta http-equiv=\"Content-Security-Policy\" content=\"script-src 'none'\">";

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// preview() must never reach the user's session bus: set before QApplication, whose platform theme may connect.
[[maybe_unused]] static const bool kNoBus = qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/omarchy-mail-test-bus");

class TstLaunch : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void open_uses_uwsm()
    {
        QTemporaryDir dir(scratch());
        const QString stub = writeStub(dir.path());
        launch::setProgramForTests(stub);
        QVERIFY(launch::open("/x/a.pdf"));
        QTRY_COMPARE(stubCalls(stub), (QList<QStringList>{{"--", "xdg-open", "/x/a.pdf"}}));
    }
    void preview_falls_back_to_open()
    {
        QTemporaryDir dir(scratch());
        const QString stub = writeStub(dir.path());
        launch::setProgramForTests(stub);
        launch::preview("/x/b.png");
        QTRY_COMPARE(stubCalls(stub), (QList<QStringList>{{"--", "xdg-open", "/x/b.png"}}));
    }
    void save_unique_name()
    {
        QTemporaryDir src(scratch()), dst(scratch());
        for (const char *name : {"a.pdf", "README"}) {
            QFile f(src.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("data");
        }
        QVERIFY(QFile::copy(src.filePath("a.pdf"), dst.filePath("a.pdf")));
        QCOMPARE(launch::saveCopy(src.filePath("a.pdf"), dst.path()), dst.filePath("a (2).pdf"));
        QCOMPARE(launch::saveCopy(src.filePath("a.pdf"), dst.path()), dst.filePath("a (3).pdf"));
        QCOMPARE(readFile(dst.filePath("a (3).pdf")), QByteArray("data"));
        QCOMPARE(launch::saveCopy(src.filePath("README"), dst.path()), dst.filePath("README"));
        QCOMPARE(launch::saveCopy(src.filePath("README"), dst.path()), dst.filePath("README (2)"));
        QCOMPARE(launch::saveCopy(src.filePath("missing"), dst.path()), QString());
    }
    void original_csp_first_and_cid_replaced()
    {
        QTemporaryDir dir(scratch());
        const QString logo = QStringLiteral(OM_SOURCE_DIR "/tests/fixtures/logo.png");
        const QJsonObject full{{"summary", QJsonObject{{"id", 42}}},
                               {"html", "<p>Hi</p><img src=\"cid:logo@x\"><img src=\"cid:logo@x2\">"},
                               {"inline", QJsonObject{{"logo@x", logo}}}};
        const QString path = launch::writeOriginal(full, dir.filePath("original"));
        QCOMPARE(path, dir.filePath("original/42.html"));
        const QByteArray data = readFile(path);
        QVERIFY2(data.startsWith(kCsp), data.left(200).constData());
        QVERIFY(data.contains("src=\"" + QUrl::fromLocalFile(logo).toEncoded() + "\""));
        QVERIFY(data.contains("src=\"cid:logo@x2\"")); // unknown cid stays; a known prefix doesn't eat it
        QCOMPARE(QFileInfo(path).permissions() & 0x7777,
                 QFile::Permissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser));
    }
    void original_plain_escaped()
    {
        QTemporaryDir dir(scratch());
        const QJsonObject full{{"summary", QJsonObject{{"id", 7}}}, {"text", "<b>hi</b>"}, {"html", QJsonValue()}};
        const QByteArray data = readFile(launch::writeOriginal(full, dir.path()));
        QVERIFY(data.startsWith(kCsp));
        const qsizetype pre = data.indexOf("<pre");
        QVERIFY(pre > 0);
        QVERIFY(data.indexOf("&lt;b&gt;hi&lt;/b&gt;") > pre);
        QVERIFY(!data.contains("<b>"));
    }
    void render_source_linkifies_plain()
    {
        const QString s = renderSource({{"text", "see https://x.org/a?b=1&c <now>"}}, {}, QColor());
        QVERIFY2(s.contains("<a href=\"https://x.org/a?b=1&amp;c\">"), qPrintable(s));
        QVERIFY(s.contains("&lt;now&gt;"));
        QVERIFY(s.startsWith("<pre style=\"white-space:pre-wrap;font-family:monospace\">"));
        QCOMPARE(renderSource({{"html", "<p>x</p>"}, {"text", "y"}}, {}, QColor()), QString("<p>x</p>"));
        QVERIFY(renderSource({{"text", "find me"}}, {"find"}, QColor(Qt::yellow)).contains("<span"));
    }
};

QTEST_MAIN(TstLaunch)
#include "tst_launch.moc"
