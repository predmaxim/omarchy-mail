#include "Images.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace om;

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Answers by path: /silent never, /big promises 11 MiB, /cookie sets one, everything else is logo.png.
class Server : public QTcpServer
{
public:
    QStringList paths;
    QStringList requests; // raw request heads
    QByteArray body = readFile(QStringLiteral(OM_SOURCE_DIR "/tests/fixtures/logo.png"));

    Server()
    {
        QVERIFY2(listen(QHostAddress::LocalHost), "listen");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [this, s] { onRead(s); });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    QUrl url(const QString &path) const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path)); }

private:
    void onRead(QTcpSocket *s)
    {
        const QByteArray head = s->readAll();
        requests << QString::fromLatin1(head);
        const QString path = QString::fromLatin1(head.split(' ').value(1));
        paths << path;
        if (path == QLatin1String("/silent"))
            return;
        if (path == QLatin1String("/big")) {
            s->write("HTTP/1.1 200 OK\r\nContent-Length: 11534336\r\n\r\n" + QByteArray(65536, 'x'));
            return;
        }
        QByteArray h = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n";
        if (path == QLatin1String("/cookie"))
            h += "Set-Cookie: a=b; Path=/\r\n";
        s->write(h + "\r\n" + body);
        s->disconnectFromHost();
    }
};

class TstImages : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fetches_and_caches()
    {
        Server srv;
        QTemporaryDir dir;
        Images img(dir.path());
        QSignalSpy spy(&img, &Images::fetched);
        const QUrl u = srv.url("/a.png");
        img.fetch({u.toString()}, 7);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.first().first().toULongLong(), 7u);
        QCOMPARE(readFile(Images::pathFor(dir.path(), u)), srv.body);
        QVERIFY(srv.requests.value(0).contains("User-Agent: omarchy-mail"));

        img.fetch({u.toString()}, 8);
        QTest::qWait(150);
        QCOMPARE(srv.paths.size(), 1);
        QCOMPARE(spy.size(), 1);
    }

    void http_https_only()
    {
        Server srv;
        QTemporaryDir dir;
        Images img(dir.path());
        QSignalSpy spy(&img, &Images::fetched);
        const QStringList urls{"ftp://127.0.0.1:1/a.png", "file:///etc/passwd", "data:image/png;base64,AAAA"};
        img.fetch(urls, 1);
        QTest::qWait(150);
        QCOMPARE(spy.size(), 0);
        QVERIFY(srv.paths.isEmpty());
        QVERIFY(QDir(dir.path()).isEmpty());
    }

    void size_cap()
    {
        Server srv;
        QTemporaryDir dir;
        Images img(dir.path());
        QSignalSpy spy(&img, &Images::fetched);
        const QUrl big = srv.url("/big"), ok = srv.url("/ok");
        img.fetch({big.toString(), ok.toString()}, 2);
        QVERIFY(spy.wait(5000)); // the batch finishes: the big one aborted, the other written
        QVERIFY(!QFile::exists(Images::pathFor(dir.path(), big)));
        QVERIFY(QFile::exists(Images::pathFor(dir.path(), ok)));
    }

    void timeout_finishes_batch()
    {
        Server srv;
        QTemporaryDir dir;
        Images img(dir.path());
        img.setTimeoutMs(200);
        QSignalSpy spy(&img, &Images::fetched);
        const QUrl slow = srv.url("/silent"), ok = srv.url("/ok");
        QElapsedTimer t;
        t.start();
        img.fetch({slow.toString(), ok.toString()}, 3);
        QVERIFY(spy.wait(2000));
        QVERIFY(t.elapsed() < 2000);
        QVERIFY(!QFile::exists(Images::pathFor(dir.path(), slow)));
        QCOMPARE(readFile(Images::pathFor(dir.path(), ok)), srv.body);
    }

    void cap_100_urls()
    {
        Server srv;
        QTemporaryDir dir;
        Images img(dir.path());
        QSignalSpy spy(&img, &Images::fetched);
        QStringList urls;
        for (int i = 0; i < 150; ++i)
            urls << srv.url(QStringLiteral("/i%1").arg(i)).toString();
        img.fetch(urls, 4);
        QVERIFY(spy.wait(15000));
        QCOMPARE(srv.paths.size(), 100);
    }

    void prune_old()
    {
        QTemporaryDir dir;
        Images img(dir.path());
        const QString oldp = dir.filePath("old"), newp = dir.filePath("new");
        for (const QString &p : {oldp, newp}) {
            QFile f(p);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
            f.close();
        }
        QFile o(oldp), n(newp);
        QVERIFY(o.open(QIODevice::ReadWrite));
        QVERIFY(o.setFileTime(QDateTime::currentDateTime().addDays(-31), QFileDevice::FileModificationTime));
        QVERIFY(n.open(QIODevice::ReadWrite));
        QVERIFY(n.setFileTime(QDateTime::currentDateTime().addDays(-1), QFileDevice::FileModificationTime));
        o.close();
        n.close();
        img.prune();
        QVERIFY(!QFile::exists(oldp));
        QVERIFY(QFile::exists(newp));
    }

    void no_cookies()
    {
        Server srv;
        QTemporaryDir dir;
        Images img(dir.path());
        QSignalSpy spy(&img, &Images::fetched);
        img.fetch({srv.url("/cookie").toString()}, 5);
        QVERIFY(spy.wait(5000));
        img.fetch({srv.url("/second").toString()}, 6);
        QVERIFY(spy.wait(5000));
        QCOMPARE(srv.requests.size(), 2);
        QVERIFY(!srv.requests.value(1).contains("Cookie:", Qt::CaseInsensitive));
    }
};

QTEST_MAIN(TstImages)
#include "tst_images.moc"
