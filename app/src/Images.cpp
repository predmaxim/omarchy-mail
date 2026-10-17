#include "Images.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QSaveFile>
#include <QStandardPaths>
#include <memory>

namespace om {

static constexpr qint64 kMaxBytes = 10 * 1024 * 1024;
static constexpr int kMaxUrls = 100;

QString Images::defaultDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/omarchy-mail/window/img");
}

QString Images::pathFor(const QString &dir, const QUrl &url)
{
    const QByteArray name = QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha1).toHex();
    return dir + QLatin1Char('/') + QString::fromLatin1(name);
}

Images::Images(QString dir, QObject *parent)
    : QObject(parent)
    , m_dir(std::move(dir))
{
    QDir().mkpath(m_dir);
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

void Images::fetch(const QStringList &urls, quint64 generation)
{
    auto batch = std::make_shared<Batch>();
    QSet<QUrl> seen;
    int taken = 0;
    for (const QString &s : urls) {
        if (taken++ >= kMaxUrls)
            break;
        const QUrl url(s);
        if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
            || seen.contains(url) || QFile::exists(pathFor(m_dir, url)))
            continue;
        seen.insert(url);

        QNetworkRequest req(url);
        req.setTransferTimeout(m_timeoutMs);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("omarchy-mail"));
        QNetworkReply *reply = m_net.get(req);
        ++batch->pending;

        // Content-Length is known with the headers; the received size covers lying or chunked servers.
        connect(reply, &QNetworkReply::metaDataChanged, reply, [reply] {
            if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > kMaxBytes)
                reply->abort();
        });
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 got, qint64) {
            if (got > kMaxBytes)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, url, batch, generation] {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                const QByteArray data = reply->readAll();
                QSaveFile f(pathFor(m_dir, url));
                if (data.size() <= kMaxBytes && f.open(QIODevice::WriteOnly) && f.write(data) == data.size()
                    && f.commit())
                    batch->wrote = true;
            }
            if (--batch->pending == 0 && batch->wrote)
                Q_EMIT fetched(generation);
        });
    }
}

void Images::prune(int days)
{
    const QDateTime limit = QDateTime::currentDateTime().addDays(-days);
    const auto files = QDir(m_dir).entryInfoList(QDir::Files);
    for (const QFileInfo &fi : files)
        if (fi.lastModified() < limit)
            QFile::remove(fi.absoluteFilePath());
}

} // namespace om
