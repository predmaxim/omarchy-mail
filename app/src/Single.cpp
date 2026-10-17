#include "Single.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>

namespace om::single {

static QString socketPath()
{
    QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (dir.isEmpty())
        dir = QDir::tempPath();
    return QDir(dir).filePath(QStringLiteral("omarchy-mail-window"));
}

bool forward(const QStringList &args)
{
    QLocalSocket s;
    s.connectToServer(socketPath());
    if (!s.waitForConnected(500))
        return false;
    s.write(QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact) + '\n');
    const bool sent = s.waitForBytesWritten(2000);
    s.disconnectFromServer();
    return sent;
}

bool Server::listen()
{
    const QString path = socketPath();
    QLocalServer::removeServer(path);   // a stale socket of a dead window; forward() found no live one
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *s = m_server.nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QLocalSocket::readyRead, this, [this, s] {
                while (s->canReadLine()) {
                    const QJsonArray a = QJsonDocument::fromJson(s->readLine()).array();
                    QStringList args;
                    for (const QJsonValue &v : a)
                        args << v.toString();
                    Q_EMIT received(args);
                }
            });
        }
    });
    return m_server.listen(path);
}

} // namespace om::single
