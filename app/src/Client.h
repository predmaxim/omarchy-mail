#pragma once
// Connection to maild: persistent JSON-lines socket (subscribed as the window) plus
// one-shot connections for slow requests. Never logs request bodies (account_add has a password).
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <functional>

namespace om {

class Client : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
public:
    using Reply = std::function<void(const QJsonValue &result, const QString &error)>;

    explicit Client(QString socketPath = defaultSocketPath(), QObject *parent = nullptr);
    ~Client() override { m_sock.disconnect(this); }
    static QString defaultSocketPath();

    bool connected() const { return m_connected; }
    void start();
    void request(QJsonObject req, Reply reply = {});
    void requestSlow(QJsonObject req, Reply reply = {});   // own connection, 90 s limit
    void setRetryInterval(int ms) { m_retry.setInterval(ms); }

Q_SIGNALS:
    void connectedChanged();
    void event(const QString &name, const QJsonObject &payload);

private:
    void send(QJsonObject req, Reply reply);
    void readable();
    void dropped();
    void connectNow();

    QString m_path;
    QLocalSocket m_sock;
    QByteArray m_buf;
    QHash<qint64, Reply> m_pending;
    qint64 m_nextId = 1;
    bool m_connected = false;
    bool m_started = false;
    QTimer m_retry;
};

} // namespace om
