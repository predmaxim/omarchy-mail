#include "FakeMaild.h"
#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPointer>
#include <QTest>
#include <QTimer>

namespace om {

FakeMaild::FakeMaild(QObject *parent) : QObject(parent)
{
    connect(&m_server, &QLocalServer::newConnection, this, &FakeMaild::accept);
}

QString FakeMaild::path() const { return m_dir.filePath(QStringLiteral("maild.sock")); }

bool FakeMaild::start()
{
    QLocalServer::removeServer(path());
    return m_server.listen(path());
}

void FakeMaild::stop()
{
    const auto socks = m_conns.keys();
    m_conns.clear();
    for (auto *s : socks) {
        s->disconnect(this);
        s->disconnectFromServer();
        s->deleteLater();
    }
    m_server.close();
}

void FakeMaild::on(const QString &cmd, Handler h) { m_handlers.insert(cmd, std::move(h)); }

void FakeMaild::accept()
{
    while (auto *s = m_server.nextPendingConnection()) {
        m_conns.insert(s, Conn{m_nextConn++, {}, false});
        connect(s, &QLocalSocket::readyRead, this, [this, s] { readable(s); });
        connect(s, &QLocalSocket::disconnected, this, [this, s] {
            m_conns.remove(s);
            s->deleteLater();
        });
    }
}

void FakeMaild::readable(QLocalSocket *s)
{
    auto it = m_conns.find(s);
    if (it == m_conns.end())
        return;
    it->buf += s->readAll();
    for (;;) {
        it = m_conns.find(s);
        if (it == m_conns.end())
            return;
        const int nl = it->buf.indexOf('\n');
        if (nl < 0)
            return;
        const QByteArray line = it->buf.left(nl).trimmed();
        it->buf.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        QJsonObject req = QJsonDocument::fromJson(line).object();
        const QJsonValue id = req.value(QStringLiteral("id"));
        req.insert(QStringLiteral("conn"), it->n);
        m_requests << req;
        const QString cmd = req.value(QStringLiteral("cmd")).toString();
        if (cmd == QLatin1String("subscribe"))
            it->subscribed = true;
        QJsonObject resp = m_handlers.contains(cmd) ? m_handlers.value(cmd)(req) : QJsonObject{};
        if (resp.contains(QStringLiteral("hang")))
            continue;
        const int delay = resp.take(QStringLiteral("delay")).toInt();
        resp.insert(QStringLiteral("id"), id);
        resp.insert(QStringLiteral("ok"), !resp.contains(QStringLiteral("error")));
        if (delay > 0) {
            QPointer<QLocalSocket> p(s);
            QTimer::singleShot(delay, this, [this, p, resp] {
                if (p && m_conns.contains(p))
                    write(p, resp);
            });
        } else {
            write(s, resp);
        }
    }
}

void FakeMaild::write(QLocalSocket *s, QJsonObject o)
{
    s->write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
}

void FakeMaild::emitEvent(const QJsonObject &ev)
{
    for (auto it = m_conns.cbegin(); it != m_conns.cend(); ++it)
        if (it->subscribed)
            write(it.key(), ev);
}

QList<QJsonObject> FakeMaild::requests() const { return m_requests; }

bool FakeMaild::waitFor(const QString &cmd, int ms)
{
    QDeadlineTimer d(ms);
    for (;;) {
        for (const auto &r : std::as_const(m_requests))
            if (r.value(QStringLiteral("cmd")).toString() == cmd)
                return true;
        if (d.hasExpired())
            return false;
        QTest::qWait(5);
    }
}

void FakeMaild::sendRaw(const QByteArray &line)
{
    for (auto it = m_conns.cbegin(); it != m_conns.cend(); ++it)
        it.key()->write(line);
}

} // namespace om
