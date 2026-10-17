#pragma once
// Test double for maild: a QLocalServer on a temp socket speaking the JSON-lines protocol.
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QObject>
#include <QTemporaryDir>
#include <functional>

class QLocalSocket;

namespace om {

class FakeMaild : public QObject {
    Q_OBJECT
public:
    // Returns the response body: {"result":...} or {"error":"..."}; extra fields are passed through.
    // Special keys (not sent): "delay" (ms) postpones the reply, "hang" never replies.
    using Handler = std::function<QJsonObject(const QJsonObject &req)>;

    explicit FakeMaild(QObject *parent = nullptr);
    ~FakeMaild() override { stop(); }
    QString path() const;
    bool start();
    void stop();
    void on(const QString &cmd, Handler h);
    void emitEvent(const QJsonObject &ev);   // to subscribed connections
    QList<QJsonObject> requests() const;      // each with "conn": connection number
    bool waitFor(const QString &cmd, int ms = 2000);
    void sendRaw(const QByteArray &line);     // to all clients, as is

private:
    struct Conn {
        int n = 0;
        QByteArray buf;
        bool subscribed = false;
    };
    void accept();
    void readable(QLocalSocket *s);
    void write(QLocalSocket *s, QJsonObject o);

    QTemporaryDir m_dir;
    QLocalServer m_server;
    QHash<QString, Handler> m_handlers;
    QHash<QLocalSocket *, Conn> m_conns;
    QList<QJsonObject> m_requests;
    int m_nextConn = 1;
};

} // namespace om
