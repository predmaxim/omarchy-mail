#pragma once
// The open letter: `message` / `open_remote`, layout via Renderer, attachments, links, copying.
#include "Renderer.h"
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <functional>

namespace om {

class Client;
class Images;
class Theme;

// HTML for the layout: the letter's html with search marks, or its text escaped, linkified, in <pre>.
QString renderSource(const QJsonObject &full, const QStringList &words, const QColor &mark);

class Letter : public QObject {
    Q_OBJECT
    Q_PROPERTY(qint64 id READ id NOTIFY changed)
    Q_PROPERTY(bool empty READ empty NOTIFY changed)
    Q_PROPERTY(QString subject READ subject NOTIFY changed)
    Q_PROPERTY(QString from READ from NOTIFY changed)
    Q_PROPERTY(QString to READ to NOTIFY changed)
    Q_PROPERTY(QString date READ date NOTIFY changed)
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY changed)
    Q_PROPERTY(int outbox READ outbox NOTIFY changed)
    Q_PROPERTY(QString sendError READ sendError NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(int linkCount READ linkCount NOTIFY changed)
    Q_PROPERTY(bool blank READ blank NOTIFY changed)
public:
    Letter(Client *client, Images *images, Renderer *renderer, Theme *theme, QObject *parent = nullptr);

    qint64 id() const { return m_id; }
    bool empty() const { return summary().isEmpty(); }   // no letter open
    QString subject() const;
    QString from() const;
    QString to() const;
    QString date() const;
    QVariantList attachments() const;   // not inline: {index, name, sizeText, icon, busy}
    int outbox() const;                 // 0 no, 1 queued, 2 not sent
    QString sendError() const { return m_full.value(QStringLiteral("send_error")).toString(); }
    QString state() const { return m_state; }   // "", "loading", "remote"
    QString error() const { return m_error; }
    int linkCount() const { return int(m_links.size()); }   // links with a box: the keyboard steps through these
    bool blank() const { return m_blank; }                   // laid out with no text and no image

    void show(const QJsonObject &summary);
    void reload();
    void clear();
    void setWidth(int px);
    void setWords(const QStringList &words);

    QJsonObject full() const { return m_full; }
    std::shared_ptr<DocumentContainer> document() const { return m_doc; }
    bool dark() const { return m_dark; }
    QList<DocumentContainer::Link> links() const { return m_links; }

    Q_INVOKABLE void openAttachment(int i);
    Q_INVOKABLE void previewAttachment(int i);
    Q_INVOKABLE void saveAttachment(int i);
    Q_INVOKABLE void openOriginal();
    Q_INVOKABLE void copyText();
    Q_INVOKABLE void activateLink(const QUrl &url);

Q_SIGNALS:
    void changed();
    void documentChanged(bool sameLetter);
    void composeTo(const QUrl &url);
    void note(const QString &text, bool error);

private:
    QJsonObject summary() const;
    void reset(const QJsonObject &summary);
    void fetch();
    void openRemote();
    void layout();
    void rendered(const Rendered &r);
    void withAttachment(int i, std::function<void(const QString &path)> use);

    Client *m_client;
    Images *m_images;
    Renderer *m_renderer;
    Theme *m_theme;
    quint64 m_gen = 0;          // message requests (show, reload): stale replies are dropped
    quint64 m_letter = 0;       // letters shown (not reloads): image batches of another letter are dropped
    quint64 m_job = 0;          // layout jobs
    quint64 m_imagesLetter = 0; // the letter whose images were fetched already
    qint64 m_id = 0;
    QJsonObject m_summary, m_full;
    QString m_state, m_error;
    QSet<int> m_busy;
    QStringList m_words;
    int m_width = 0;
    std::shared_ptr<DocumentContainer> m_doc;
    bool m_dark = false, m_blank = false;
    QList<DocumentContainer::Link> m_links;
    QTimer m_remoteTimer, m_widthTimer;
};

} // namespace om
