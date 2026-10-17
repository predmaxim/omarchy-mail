#pragma once
#include <QColor>
#include <QFont>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QStringList>
#include <QThreadPool>
#include <container_qpainter.h>
#include <atomic>
#include <memory>

namespace om {

struct RenderJob
{
    quint64 generation = 0;
    QString html;
    QHash<QString, QString> inlinePaths; // Full.inline: cid -> file
    QString imageDir;                    // Images::defaultDir()
    int width = 0;
    int viewportHeight = 0;
    QFont font;
    QColor selection;
    QColor background, foreground, muted, dim; // theme: quotes are painted muted and dim
    QColor accent;                              // theme: the "Show quoted text" link
    QString quotesLabel;                        // t("Show quoted text")
    QSet<int> expanded;                         // tall top-level quotes shown in full (0 = the first quote)
};

struct Rendered
{
    quint64 generation = 0;
    std::shared_ptr<DocumentContainer> doc;
    bool dark = false;
    QStringList wanted; // remote images not in the cache yet
    QList<DocumentContainer::Link> links;
    QSize size;
    bool blank = false; // no text and no image: nothing to show
};

// Data callback of the layout thread: cid:, data:image/*, cached http(s); never the network.
QByteArray loadData(const QUrl &url, const RenderJob &job, QStringList *wanted);

// Lays letters out in its own thread; only the latest generation is delivered.
class Renderer : public QObject
{
    Q_OBJECT

public:
    explicit Renderer(QObject *parent = nullptr);
    ~Renderer() override;
    void render(const RenderJob &job);

Q_SIGNALS:
    void rendered(const om::Rendered &result);

private:
    QThreadPool m_pool;
    std::atomic<quint64> m_latest{0};
};

} // namespace om

Q_DECLARE_METATYPE(om::Rendered)
