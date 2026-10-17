#pragma once
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QUrl>

namespace om {

// The window's only network: images of the open letter, into a file cache.
class Images : public QObject
{
    Q_OBJECT

public:
    explicit Images(QString dir = defaultDir(), QObject *parent = nullptr);
    static QString defaultDir();
    // Pure: the layout thread calls it.
    static QString pathFor(const QString &dir, const QUrl &url);
    QString dir() const { return m_dir; }

    void fetch(const QStringList &urls, quint64 generation);
    void prune(int days = 30);
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }

Q_SIGNALS:
    void fetched(quint64 generation);

private:
    struct Batch
    {
        int pending = 0;
        bool wrote = false;
    };
    QString m_dir;
    int m_timeoutMs = 15000;
    QNetworkAccessManager m_net;
};

} // namespace om
