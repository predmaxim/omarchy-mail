#include "AppHarness.h"
#include "Single.h"
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>

using namespace om;
using namespace om::test;

// One window per system: the first omarchy-mail listens on $XDG_RUNTIME_DIR/omarchy-mail-window, the second hands
// its arguments over and exits 0. Real binaries, offscreen, in a temporary runtime dir on a FakeMaild.
class TstSingle : public QObject {
    Q_OBJECT

    static QProcessEnvironment env(const QString &root)
    {
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        for (const char *k : {"QT_QPA_PLATFORMTHEME", "HYPRLAND_INSTANCE_SIGNATURE", "WAYLAND_DISPLAY", "DISPLAY"})
            e.remove(QLatin1String(k));
        e.insert("QT_QPA_PLATFORM", "offscreen");
        e.insert("QT_QUICK_BACKEND", "software");
        e.insert("HOME", root);
        e.insert("XDG_RUNTIME_DIR", root + "/r");
        e.insert("XDG_DATA_HOME", root + "/d");
        e.insert("XDG_CACHE_HOME", root + "/c");
        e.insert("XDG_STATE_HOME", root + "/s");
        e.insert("XDG_CONFIG_HOME", root + "/f");
        e.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + root + "/no-bus");
        return e;
    }
    static QString draft(const QString &root)
    {
        QFile f(root + "/d/omarchy-mail/drafts/new.json");
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

private Q_SLOTS:
    void forward_roundtrip()
    {
        QTemporaryDir dir(scratch());
        qputenv("XDG_RUNTIME_DIR", dir.path().toUtf8());
        QCOMPARE(single::forward({"x"}), false);   // nobody listens
        single::Server server;
        QVERIFY(server.listen());
        QSignalSpy got(&server, &single::Server::received);
        QVERIFY(single::forward({"--open", "7", "mailto:a@b.c?subject=Привет"}));
        QTRY_COMPARE(got.size(), 1);
        QCOMPARE(got.at(0).at(0).toStringList(), (QStringList{"--open", "7", "mailto:a@b.c?subject=Привет"}));
    }

    void second_instance_forwards_mailto()
    {
        AppHarness h;
        h.serve();
        QDir().mkpath(QDir::homePath() + "/.cache/omwork");
        QTemporaryDir dir(QDir::homePath() + "/.cache/omwork/s-XXXXXX");   // short: sun_path
        const QString root = dir.path();
        QVERIFY(QDir().mkpath(root + "/r"));
        QFile::setPermissions(root + "/r", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QVERIFY(QFile::link(h.d->path(), root + "/r/omarchy-mail.sock"));
        QVERIFY(root.size() + 24 < 100);   // sun_path

        QProcess first;
        first.setProcessEnvironment(env(root));
        first.setProcessChannelMode(QProcess::ForwardedChannels);
        first.start(OM_APP_BINARY, {});
        QVERIFY(first.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(root + "/r/omarchy-mail-window"), 10000);

        QProcess second;
        second.setProcessEnvironment(env(root));
        second.start(OM_APP_BINARY, {"mailto:b@y.org?subject=Hi%20there&body=text"});
        QVERIFY(second.waitForFinished(10000));
        QCOMPARE(second.exitStatus(), QProcess::NormalExit);
        QCOMPARE(second.exitCode(), 0);

        QTRY_VERIFY_WITH_TIMEOUT(draft(root).contains("b@y.org"), 10000);   // the first window's draft, saved after 1 s
        const QString d = draft(root);
        QVERIFY2(d.contains("Hi there") && d.contains("text") && d.contains("me@gmail.com"), qPrintable(d));
        QCOMPARE(first.state(), QProcess::Running);
        first.kill();
        first.waitForFinished(5000);
    }
};

QTEST_MAIN(TstSingle)
#include "tst_single.moc"
