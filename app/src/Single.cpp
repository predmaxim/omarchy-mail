#include "Single.h"
#include "Client.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLockFile>
#include <QTimer>

namespace om::single {

static constexpr qint64 kMaxLine = 64 * 1024;

static QString socketPath() { return QDir(Client::runtimeDir()).filePath(QStringLiteral("omarchy-mail-window")); }

// {"args":[...]}; anything else is not ours.
static bool parse(const QByteArray &line, QStringList *out)
{
    const QJsonValue a = QJsonDocument::fromJson(line).object().value(QStringLiteral("args"));
    if (!a.isArray())
        return false;
    out->clear();
    for (const QJsonValue &v : a.toArray()) {
        if (!v.isString())
            return false;
        *out << v.toString();
    }
    return true;
}

Server::Claim Server::claim(const QStringList &args)
{
    const QString path = socketPath();
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *s = m_server.nextPendingConnection()) {
            s->setReadBufferSize(kMaxLine);
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            QTimer::singleShot(2000, s, [s] { s->abort(); });   // a peer that never finishes its line
            connect(s, &QLocalSocket::readyRead, this, [this, s] {
                while (s->canReadLine()) {
                    QStringList a;
                    if (parse(s->readLine(), &a))
                        Q_EMIT received(a);
                }
                if (s->bytesAvailable() >= kMaxLine)   // no newline in 64 KiB
                    s->abort();
            });
        }
    });
    // QLocalServer::listen() takes over a name that is already in use, so look first. The lock makes two first
    // launches take turns: the later one finds the earlier one's socket.
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.tryLock(3000);   // on failure go on: the connect below still protects a live window
    QLocalSocket s;
    s.connectToServer(path);
    if (s.waitForConnected(500)) {
        s.write(QJsonDocument(QJsonObject{{"args", QJsonArray::fromStringList(args)}}).toJson(QJsonDocument::Compact) + '\n');
        const bool sent = s.waitForBytesWritten(2000);
        s.disconnectFromServer();
        return sent ? Forwarded : Unreachable;
    }
    if (s.error() == QLocalSocket::ConnectionRefusedError)
        QLocalServer::removeServer(path);   // a dead window's socket file
    else if (s.error() != QLocalSocket::ServerNotFoundError)
        return Unreachable;   // frozen or busy, not dead: never take its name
    return m_server.listen(path) ? Listening : Unreachable;
}

} // namespace om::single
