#include "Client.h"
#include "FakeMaild.h"
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>

using namespace om;

struct Got {
    bool done = false;
    QJsonValue result;
    QString error;
    Client::Reply cb() { return [this](const QJsonValue &r, const QString &e) { done = true; result = r; error = e; }; }
};

class TstClient : public QObject {
    Q_OBJECT
    FakeMaild *d = nullptr;
    Client *c = nullptr;

    void connectClient()
    {
        c->start();
        QTRY_VERIFY_WITH_TIMEOUT(c->connected(), 2000);
    }
private Q_SLOTS:
    void init()
    {
        d = new FakeMaild;
        QVERIFY(d->start());
        c = new Client(d->path());
        c->setRetryInterval(100);
    }
    void cleanup()
    {
        delete c;
        delete d;
    }
    void subscribes_as_window()
    {
        connectClient();
        const auto r = d->requests();
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].value("cmd").toString(), QStringLiteral("subscribe"));
        QCOMPARE(r[0].value("window").toBool(), true);
    }
    void reply_matched_by_id()
    {
        d->on("a", [](const QJsonObject &) { return QJsonObject{{"result", 1}}; });
        d->on("b", [](const QJsonObject &) { return QJsonObject{{"result", "two"}}; });
        connectClient();
        Got a, b;
        c->request({{"cmd", "a"}}, a.cb());
        c->request({{"cmd", "b"}}, b.cb());
        QTRY_VERIFY_WITH_TIMEOUT(a.done && b.done, 2000);
        QCOMPARE(a.result.toInt(), 1);
        QCOMPARE(b.result.toString(), QStringLiteral("two"));
        QVERIFY(a.error.isEmpty() && b.error.isEmpty());
    }
    void error_reply_passed()
    {
        d->on("a", [](const QJsonObject &) { return QJsonObject{{"error", "no such account"}}; });
        connectClient();
        Got a;
        c->request({{"cmd", "a"}}, a.cb());
        QTRY_VERIFY_WITH_TIMEOUT(a.done, 2000);
        QCOMPARE(a.error, QStringLiteral("no such account"));
    }
    void events_between_responses()
    {
        d->on("a", [this](const QJsonObject &) {
            d->emitEvent({{"event", "changed"}, {"folders", true}, {"ids", QJsonArray{1}}});
            return QJsonObject{{"result", 1}};
        });
        connectClient();
        QSignalSpy spy(c, &Client::event);
        Got a, b;
        c->request({{"cmd", "a"}}, a.cb());
        c->request({{"cmd", "a"}}, b.cb());
        QTRY_VERIFY_WITH_TIMEOUT(a.done && b.done, 2000);
        QVERIFY(spy.count() >= 1);
        QCOMPARE(spy[0][0].toString(), QStringLiteral("changed"));
        QCOMPARE(spy[0][1].toJsonObject().value("ids").toArray(), QJsonArray{1});
    }
    void blank_lines_and_unknown_fields_ignored()
    {
        d->on("a", [this](const QJsonObject &) {
            d->sendRaw("\n");
            return QJsonObject{{"result", 7}, {"x", 1}};
        });
        connectClient();
        Got a;
        c->request({{"cmd", "a"}}, a.cb());
        QTRY_VERIFY_WITH_TIMEOUT(a.done, 2000);
        QCOMPARE(a.result.toInt(), 7);
        QVERIFY(a.error.isEmpty());
        QVERIFY(c->connected());
    }
    void pending_fail_on_disconnect()
    {
        d->on("a", [](const QJsonObject &) { return QJsonObject{{"hang", true}}; });
        connectClient();
        QSignalSpy cs(c, &Client::connectedChanged);
        Got a;
        c->request({{"cmd", "a"}}, a.cb());
        QVERIFY(d->waitFor("a"));
        d->stop();
        QTRY_VERIFY_WITH_TIMEOUT(a.done, 2000);
        QCOMPARE(a.error, QStringLiteral("Mail service is not running"));
        QVERIFY(!c->connected());
        QVERIFY(cs.count() >= 1);
    }
    void request_without_connection()
    {
        Got a;
        c->request({{"cmd", "a"}}, a.cb());
        QVERIFY(a.done);
        QCOMPARE(a.error, QStringLiteral("Mail service is not running"));
    }
    void reconnects_after_server_restart()
    {
        connectClient();
        d->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!c->connected(), 2000);
        QVERIFY(d->start());
        QTRY_VERIFY_WITH_TIMEOUT(c->connected(), 2000);
        int subs = 0;
        for (const auto &r : d->requests())
            subs += r.value("cmd").toString() == QLatin1String("subscribe");
        QCOMPARE(subs, 2);
    }
    void slow_request_uses_own_connection()
    {
        d->on("message", [](const QJsonObject &) { return QJsonObject{{"result", 5}, {"delay", 300}}; });
        d->on("ping", [](const QJsonObject &) { return QJsonObject{{"result", 1}}; });
        connectClient();
        Got slow, fast;
        c->requestSlow({{"cmd", "message"}, {"message", 5}}, slow.cb());
        QVERIFY(d->waitFor("message"));
        c->request({{"cmd", "ping"}}, fast.cb());
        QTRY_VERIFY_WITH_TIMEOUT(fast.done, 2000);
        QVERIFY(!slow.done);
        QTRY_VERIFY_WITH_TIMEOUT(slow.done, 2000);
        QCOMPARE(slow.result.toInt(), 5);
        int subConn = -1, msgConn = -1;
        for (const auto &r : d->requests()) {
            if (r.value("cmd").toString() == QLatin1String("subscribe"))
                subConn = r.value("conn").toInt();
            if (r.value("cmd").toString() == QLatin1String("message"))
                msgConn = r.value("conn").toInt();
        }
        QVERIFY(subConn > 0 && msgConn > 0);
        QVERIFY(subConn != msgConn);
    }
    void slow_request_without_server()
    {
        d->stop();
        Got a;
        c->requestSlow({{"cmd", "message"}}, a.cb());
        QTRY_VERIFY_WITH_TIMEOUT(a.done, 2000);
        QCOMPARE(a.error, QStringLiteral("Mail service is not running"));
    }
};

QTEST_MAIN(TstClient)
#include "tst_client.moc"
