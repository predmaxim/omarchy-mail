#include "Client.h"
#include "I18n.h"
#include <QDir>
#include <QJsonDocument>
#include <QPointer>
#include <cstdio>
#include <unistd.h>

namespace om {

namespace {
constexpr qsizetype kMaxBuf = 16 << 20;
// Reply from one parsed response object.
void deliver(const Client::Reply &r, const QJsonObject &o)
{
    if (!r)
        return;
    if (o.value(QStringLiteral("ok")).toBool()) {
        r(o.value(QStringLiteral("result")), {});
    } else {
        QString e = o.value(QStringLiteral("error")).toString();
        r({}, e.isEmpty() ? t("Request failed") : e);
    }
}
QByteArray line(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'; }
} // namespace

Client::Client(QString socketPath, QObject *parent) : QObject(parent), m_path(std::move(socketPath))
{
    m_retry.setSingleShot(true);
    m_retry.setInterval(2000);
    connect(&m_retry, &QTimer::timeout, this, &Client::connectNow);
    connect(&m_sock, &QLocalSocket::connected, this, [this] {
        send({{"cmd", "subscribe"}, {"window", true}}, [this](const QJsonValue &, const QString &err) {
            if (!err.isEmpty()) {
                dropped();
                return;
            }
            if (m_connected)
                return;
            m_connected = true;
            Q_EMIT connectedChanged();
        });
    });
    connect(&m_sock, &QLocalSocket::readyRead, this, &Client::readable);
    connect(&m_sock, &QLocalSocket::disconnected, this, &Client::dropped);
    connect(&m_sock, &QLocalSocket::errorOccurred, this, &Client::dropped);
}

Client::~Client()
{
    m_sock.disconnect(this);
    // in-flight slow requests: no reply may run during destruction
    const auto kids = findChildren<QLocalSocket *>(Qt::FindDirectChildrenOnly);
    for (auto *k : kids)
        k->disconnect();
}

QString Client::runtimeDir()
{
    const QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return dir.isEmpty() ? QStringLiteral("/run/user/%1").arg(getuid()) : dir;
}

QString Client::defaultSocketPath()
{
    return QDir(runtimeDir()).filePath(QStringLiteral("omarchy-mail.sock"));
}

void Client::start()
{
    m_started = true;
    connectNow();
}

void Client::connectNow()
{
    if (m_sock.state() == QLocalSocket::UnconnectedState)
        m_sock.connectToServer(m_path);
}

void Client::dropped()
{
    m_buf.clear();
    m_sock.abort();   // idempotent; no further signals for an unconnected socket
    const bool was = std::exchange(m_connected, false);   // before the drain: callbacks' request() fails at once
    auto pending = std::exchange(m_pending, {});
    for (const auto &r : std::as_const(pending))
        if (r)
            r({}, t("Mail service is not running"));
    if (was)
        Q_EMIT connectedChanged();
    if (m_started && !m_retry.isActive())
        m_retry.start();
}

void Client::send(QJsonObject req, Reply reply)
{
    const qint64 id = m_nextId++;
    req.insert(QStringLiteral("id"), id);
    m_pending.insert(id, std::move(reply));
    m_sock.write(line(req));
}

void Client::request(QJsonObject req, Reply reply)
{
    if (!m_connected) {
        if (reply)
            reply({}, t("Mail service is not running"));
        return;
    }
    send(std::move(req), std::move(reply));
}

void Client::readable()
{
    m_buf += m_sock.readAll();
    if (m_buf.size() > kMaxBuf) {
        dropped();
        return;
    }
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        const QByteArray l = m_buf.left(nl).trimmed();
        m_buf.remove(0, nl + 1);
        if (l.isEmpty())
            continue;
        const QJsonObject o = QJsonDocument::fromJson(l).object();
        if (o.contains(QStringLiteral("event"))) {
            Q_EMIT event(o.value(QStringLiteral("event")).toString(), o);
        } else if (o.contains(QStringLiteral("id"))) {
            const auto id = o.value(QStringLiteral("id")).toInteger(-1);
            if (m_pending.contains(id)) {
                const Reply r = m_pending.take(id);
                deliver(r, o);
            }
        } else {
            std::fputs("om: unrecognised line from maild\n", stderr);
        }
    }
}

void Client::requestSlow(QJsonObject req, Reply reply)
{
    auto *s = new QLocalSocket(this);
    auto *timer = new QTimer(s);
    auto buf = std::make_shared<QByteArray>();
    auto done = std::make_shared<bool>(false);
    const auto finish = [s, done, reply = std::move(reply)](const QJsonObject &o, const QString &err) {
        if (*done)
            return;
        *done = true;
        s->disconnect();
        s->abort();
        s->deleteLater();
        if (reply) {
            if (err.isEmpty())
                deliver(reply, o);
            else
                reply({}, err);
        }
    };
    req.insert(QStringLiteral("id"), 1);
    connect(timer, &QTimer::timeout, s, [finish] { finish({}, t("Mail service did not answer")); });
    timer->start(90000);
    connect(s, &QLocalSocket::connected, s, [s, req] { s->write(line(req)); });
    connect(s, &QLocalSocket::errorOccurred, s, [finish] { finish({}, t("Mail service is not running")); });
    connect(s, &QLocalSocket::disconnected, s, [finish] { finish({}, t("Mail service is not running")); });
    connect(s, &QLocalSocket::readyRead, s, [s, buf, finish] {
        *buf += s->readAll();
        if (buf->size() > kMaxBuf) {
            finish({}, t("Request failed"));
            return;
        }
        int nl;
        while ((nl = buf->indexOf('\n')) >= 0) {
            const QByteArray l = buf->left(nl).trimmed();
            buf->remove(0, nl + 1);
            if (!l.isEmpty()) {
                finish(QJsonDocument::fromJson(l).object(), {});
                return;
            }
        }
    });
    s->connectToServer(m_path);
}

} // namespace om
