#include "AppHarness.h"
#include "Session.h"
#include "Single.h"
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
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
    // A temporary runtime dir for this test process; the server and clients below use it.
    QString runtime(QTemporaryDir &dir)
    {
        qputenv("XDG_RUNTIME_DIR", dir.path().toUtf8());
        return dir.path() + "/omarchy-mail-window";
    }

    void forward_roundtrip()
    {
        QTemporaryDir dir(scratch());
        runtime(dir);
        single::Server first, second;
        QCOMPARE(first.claim({}), single::Server::Listening);
        QSignalSpy got(&first, &single::Server::received);
        QCOMPARE(second.claim({"--open", "7", "mailto:a@b.c?subject=Привет"}), single::Server::Forwarded);
        QTRY_COMPARE(got.size(), 1);
        QCOMPARE(got.at(0).at(0).toStringList(), (QStringList{"--open", "7", "mailto:a@b.c?subject=Привет"}));
    }

    void stale_socket_is_replaced()
    {
        QTemporaryDir dir(scratch());
        const QString path = runtime(dir);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);   // bound, never listening: what a dead window leaves
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.toUtf8().constData(), sizeof addr.sun_path - 1);
        QCOMPARE(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr), 0);
        ::close(fd);
        QVERIFY(QFileInfo::exists(path));
        single::Server s;
        QCOMPARE(s.claim({}), single::Server::Listening);
    }

    void frozen_window_is_not_stolen()
    {
        QTemporaryDir dir(scratch());
        const QString path = runtime(dir);
        QLocalServer frozen;   // no event loop runs here: it never accepts
        QVERIFY(frozen.listen(path));
        QList<QLocalSocket *> fill;   // fill the backlog until a connect times out
        for (int i = 0; i < 500; ++i) {
            fill << new QLocalSocket(this);
            fill.last()->connectToServer(path);
            if (!fill.last()->waitForConnected(50))
                break;
        }
        QVERIFY2(!fill.last()->isOpen(), "backlog did not fill");
        single::Server s;
        QCOMPARE(s.claim({}), single::Server::Unreachable);
        QVERIFY(QFileInfo::exists(path));   // not removed
        qDeleteAll(fill);
    }

    void junk_is_ignored_and_input_is_bounded()
    {
        QTemporaryDir dir(scratch());
        const QString path = runtime(dir);
        single::Server server;
        QCOMPARE(server.claim({}), single::Server::Listening);
        QSignalSpy got(&server, &single::Server::received);
        QLocalSocket c;
        c.connectToServer(path);
        QVERIFY(c.waitForConnected(1000));
        c.write("garbage\n{\"args\":[1]}\n{\"args\":[]}\n");
        QVERIFY(c.waitForBytesWritten(1000));
        QTRY_COMPARE(got.size(), 1);   // only the well-formed one, an empty launch
        QVERIFY(got.at(0).at(0).toStringList().isEmpty());
        QLocalSocket flood;
        flood.connectToServer(path);
        QVERIFY(flood.waitForConnected(1000));
        flood.write(QByteArray(200 * 1024, 'x'));
        flood.waitForBytesWritten(1000);
        QTRY_COMPARE(flood.state(), QLocalSocket::UnconnectedState);   // cut off, no newline
        QCOMPARE(got.size(), 1);
    }

    void junk_args_do_not_raise()
    {
        QVERIFY(Session::parseArgs({}).raise);
        QVERIFY(Session::parseArgs({"mailto:a@b.c"}).raise);
        QVERIFY(Session::parseArgs({"--open", "5"}).raise);
        QVERIFY(!Session::parseArgs({"junk"}).raise);
        QVERIFY(!Session::parseArgs({"--open", "x"}).raise);
    }

    void help_exits_without_window()
    {
        QTemporaryDir dir(scratch());
        QProcess p;
        p.setProcessEnvironment(env(dir.path()));
        p.start(OM_APP_BINARY, {"--help"});
        QVERIFY(p.waitForFinished(10000));
        QCOMPARE(p.exitCode(), 0);
        QVERIFY(p.readAllStandardOutput().contains("--open"));
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
