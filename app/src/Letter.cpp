#include "Letter.h"
#include "Client.h"
#include "Format.h"
#include "Highlight.h"
#include "I18n.h"
#include "Images.h"
#include "Launch.h"
#include "Theme.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextDocumentFragment>
#include <QtConcurrentRun>

namespace om {

QString renderSource(const QJsonObject &full, const QStringList &words, const QColor &mark)
{
    const QString html = full.value(QStringLiteral("html")).toString();
    if (!html.isEmpty())
        return highlight::html(html, words, mark);
    static const QRegularExpression link(QStringLiteral("https?://[^\\s<>\"]+"));
    QString out = QStringLiteral("<pre style=\"white-space:pre-wrap;font-family:monospace\">");
    const auto add = [&](const QString &text) {   // escaped, links made
        qsizetype done = 0;
        for (auto it = link.globalMatch(text); it.hasNext();) {
            const auto m = it.next();
            const QString url = m.captured().toHtmlEscaped();
            out += text.mid(done, m.capturedStart() - done).toHtmlEscaped() + QStringLiteral("<a href=\"") + url
                + QStringLiteral("\">") + url + QStringLiteral("</a>");
            done = m.capturedEnd();
        }
        out += text.mid(done).toHtmlEscaped();
    };
    // lines starting with ">" (or "> >", ">>") are quotes: nested <blockquote>s without the marks
    int level = 0;
    QStringList run;
    const QStringList lines = full.value(QStringLiteral("text")).toString().split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        int depth = 0;
        qsizetype from = 0;
        for (qsizetype i = 0;; ++i) {
            while (i < line.size() && line.at(i) == QLatin1Char(' '))
                ++i;
            if (i == line.size() || line.at(i) != QLatin1Char('>'))
                break;
            ++depth;
            from = i + 1;
        }
        if (depth != level) {
            add(run.join(QLatin1Char('\n')));
            run.clear();
            out += depth > level ? QStringLiteral("<blockquote>").repeated(depth - level)
                                 : QStringLiteral("</blockquote>").repeated(level - depth);
            level = depth;
        }
        if (depth && from < line.size() && line.at(from) == QLatin1Char(' '))
            ++from;   // "> text": the space after the mark goes too
        run << line.mid(from);
    }
    add(run.join(QLatin1Char('\n')));
    out += QStringLiteral("</blockquote>").repeated(level) + QStringLiteral("</pre>");
    return highlight::html(out, words, mark);
}

Letter::Letter(Client *client, Images *images, Renderer *renderer, Theme *theme, QObject *parent)
    : QObject(parent), m_client(client), m_images(images), m_renderer(renderer), m_theme(theme)
{
    m_remoteTimer.setSingleShot(true);
    m_remoteTimer.setInterval(300);
    connect(&m_remoteTimer, &QTimer::timeout, this, &Letter::openRemote);
    m_widthTimer.setSingleShot(true);
    m_widthTimer.setInterval(150);
    connect(&m_widthTimer, &QTimer::timeout, this, &Letter::layout);
    connect(renderer, &Renderer::rendered, this, &Letter::rendered);
    connect(images, &Images::fetched, this, [this](quint64 letter) {
        if (letter == m_letter)
            layout();
    });
    connect(theme, &Theme::changed, this, &Letter::layout);   // font size
}

// The full letter's summary once loaded, the list row's until then.
QJsonObject Letter::summary() const
{
    return m_full.isEmpty() ? m_summary : m_full.value(QStringLiteral("summary")).toObject();
}

QString Letter::subject() const { return summary().value(QStringLiteral("subject")).toString(); }

QString Letter::from() const
{
    const QJsonObject a = summary().value(QStringLiteral("from")).toObject();
    const QString name = a.value(QStringLiteral("name")).toString(), addr = a.value(QStringLiteral("addr")).toString();
    return name.isEmpty() ? addr : name + QStringLiteral(" <") + addr + QLatin1Char('>');
}

QString Letter::to() const
{
    QStringList out;
    const QJsonArray to = m_full.value(QStringLiteral("to")).toArray();
    for (const QJsonValue &v : to)
        out << v.toObject().value(QStringLiteral("addr")).toString();
    return out.join(QStringLiteral(", "));
}

QString Letter::date() const
{
    const qint64 ts = summary().value(QStringLiteral("date")).toInteger();
    return ts ? format::letterDate(ts, QDateTime::currentDateTime(), timeLocale()) : QString();
}

int Letter::outbox() const
{
    const QJsonObject s = summary();
    if (!s.value(QStringLiteral("queued")).toBool())
        return 0;
    return s.value(QStringLiteral("failed")).toBool() ? 2 : 1;
}

QVariantList Letter::attachments() const
{
    QVariantList out;
    const QJsonArray all = m_full.value(QStringLiteral("attachments")).toArray();
    for (const QJsonValue &v : all) {
        const QJsonObject a = v.toObject();
        if (a.value(QStringLiteral("inline")).toBool())
            continue;
        const int index = a.value(QStringLiteral("index")).toInt();
        const bool image = a.value(QStringLiteral("mime")).toString().startsWith(QStringLiteral("image/"));
        out << QVariantMap{{"index", index},
                           {"name", a.value(QStringLiteral("name")).toString()},
                           {"sizeText", format::size(a.value(QStringLiteral("size")).toInteger())},
                           {"icon", image ? QStringLiteral("󰋩") : QStringLiteral("󰈙")},
                           {"busy", m_busy.contains(index)}};
    }
    return out;
}

void Letter::reset(const QJsonObject &summary)
{
    ++m_gen;
    ++m_letter;
    ++m_job;   // a layout in flight belongs to the previous letter
    m_remoteTimer.stop();
    m_widthTimer.stop();
    m_summary = summary;
    m_full = {};
    m_id = summary.value(QStringLiteral("id")).toInteger();
    m_state.clear();
    m_error.clear();
    m_busy.clear();
    m_links.clear();
    m_expanded.clear();
    m_dark = m_blank = false;
    if (std::exchange(m_doc, nullptr))
        Q_EMIT documentChanged(false);
}

void Letter::show(const QJsonObject &summary)
{
    reset(summary);
    if (m_id != 0) {
        m_state = QStringLiteral("loading");
        fetch();
    } else if (summary.contains(QStringLiteral("remote"))) {
        m_state = QStringLiteral("remote");
        m_remoteTimer.start();   // a search hit is opened only once the cursor rests on it
    }
    Q_EMIT changed();
}

void Letter::reload()
{
    if (m_id == 0)
        return;
    ++m_gen;
    fetch();
}

void Letter::clear()
{
    reset({});
    Q_EMIT changed();
}

void Letter::fetch()
{
    QPointer<Letter> self(this);
    const quint64 gen = m_gen;
    m_client->requestSlow({{"cmd", "message"}, {"message", m_id}}, [self, gen](const QJsonValue &result, const QString &err) {
        if (!self || gen != self->m_gen)
            return;
        self->m_state.clear();
        self->m_error = err;
        if (err.isEmpty())
            self->m_full = result.toObject();
        Q_EMIT self->changed();
        if (err.isEmpty())
            self->layout();
    });
}

void Letter::openRemote()
{
    const QJsonObject r = m_summary.value(QStringLiteral("remote")).toObject();
    QPointer<Letter> self(this);
    const quint64 gen = m_gen;
    m_client->requestSlow({{"cmd", "open_remote"},
                           {"account", r.value(QStringLiteral("account"))},
                           {"folder_path", r.value(QStringLiteral("folder_path"))},
                           {"uid", r.value(QStringLiteral("uid"))}},
                          [self, gen](const QJsonValue &result, const QString &err) {
                              if (!self || gen != self->m_gen)
                                  return;
                              if (!err.isEmpty()) {
                                  self->m_state.clear();
                                  self->m_error = err;
                                  Q_EMIT self->changed();
                                  return;
                              }
                              self->m_id = result.toObject().value(QStringLiteral("id")).toInteger();
                              Q_EMIT self->changed();
                              self->fetch();
                          });
}

void Letter::setWidth(int px)
{
    if (px == m_width)
        return;
    m_width = px;
    if (m_doc)
        m_widthTimer.start();
    else
        layout();
}

void Letter::setWords(const QStringList &words)
{
    if (words == m_words)
        return;
    m_words = words;
    layout();
}

void Letter::layout()
{
    if (m_full.isEmpty() || m_width <= 0)
        return;
    RenderJob job;
    job.generation = ++m_job;
    job.html = renderSource(m_full, m_words, m_theme->mark());
    const QJsonObject files = m_full.value(QStringLiteral("inline")).toObject();
    for (auto it = files.begin(); it != files.end(); ++it)
        job.inlinePaths.insert(it.key(), it.value().toString());
    job.imageDir = m_images->dir();
    job.width = m_width;   // viewportHeight stays 0: a viewport would cut html,body{height:100%;overflow:hidden}
    job.font = QFont(QStringLiteral("sans-serif"));
    job.font.setPixelSize(m_theme->fontBody());
    job.selection = m_theme->selection();
    job.background = m_theme->background();
    job.foreground = m_theme->foreground();
    job.muted = m_theme->muted();
    job.dim = m_theme->dim();
    job.expanded = m_expanded;
    m_renderer->render(job);
}

void Letter::rendered(const Rendered &r)
{
    if (r.generation != m_job)
        return;
    const bool same = bool(m_doc);
    m_doc = r.doc;
    m_dark = r.dark;
    m_blank = r.blank;
    m_links.clear();
    for (const DocumentContainer::Link &l : r.links)
        if (!l.rect.isEmpty())   // an empty anchor, or an image one whose image isn't there: nowhere to go
            m_links << l;
    // once per letter: a reload or resize neither repeats a batch in flight nor retries failed images
    if (!r.wanted.isEmpty() && m_imagesLetter != m_letter) {
        m_imagesLetter = m_letter;
        m_images->fetch(r.wanted, m_letter);
    }
    Q_EMIT changed();
    Q_EMIT documentChanged(same);
}

void Letter::withAttachment(int i, std::function<void(const QString &path)> use)
{
    if (m_id < 0) {
        Q_EMIT note(t("Attachments of unsent mail can't be opened"), true);
        return;
    }
    const QVariantMap a = attachments().value(i).toMap();
    if (a.isEmpty() || a.value(QStringLiteral("busy")).toBool())
        return;
    const int index = a.value(QStringLiteral("index")).toInt();
    const qint64 id = m_id;
    m_busy.insert(index);
    Q_EMIT changed();
    QPointer<Letter> self(this);
    m_client->requestSlow({{"cmd", "attachment"}, {"message", id}, {"index", index}},
                          [self, id, index, use = std::move(use)](const QJsonValue &result, const QString &err) {
                              if (!self)
                                  return;
                              if (self->m_id == id && self->m_busy.remove(index))
                                  Q_EMIT self->changed();
                              if (!err.isEmpty()) {
                                  Q_EMIT self->note(err, true);
                                  return;
                              }
                              use(result.toObject().value(QStringLiteral("path")).toString());
                          });
}

void Letter::openAttachment(int i)
{
    withAttachment(i, [](const QString &path) { launch::open(path); });
}

void Letter::previewAttachment(int i)
{
    withAttachment(i, [](const QString &path) { launch::preview(path); });
}

void Letter::saveAttachment(int i)
{
    withAttachment(i, [this](const QString &path) {
        const QString dest = launch::saveCopy(path, launch::downloadsDir());
        Q_EMIT note(dest.isEmpty() ? t("Could not save the file") : t("Saved to %1").arg(dest), dest.isEmpty());
    });
}

void Letter::openOriginal()
{
    if (m_full.isEmpty())
        return;
    const QString path = launch::writeOriginal(
        m_full, QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                    + QStringLiteral("/omarchy-mail/window/original"));
    if (path.isEmpty())
        Q_EMIT note(t("Could not save the file"), true);
    else
        launch::open(path);
}

void Letter::copyText()
{
    const QString text = m_full.value(QStringLiteral("text")).toString();
    if (!text.isEmpty()) {
        QGuiApplication::clipboard()->setText(text);
        return;
    }
    const QString html = m_full.value(QStringLiteral("html")).toString();
    if (html.isEmpty())
        return;
    // parsing a big letter is not for the main thread
    QtConcurrent::run([html] { return QTextDocumentFragment::fromHtml(html).toPlainText(); })
        .then(this, [](const QString &plain) { QGuiApplication::clipboard()->setText(plain); });
}

void Letter::activateLink(const QUrl &url)
{
    const QString scheme = url.scheme();
    if (scheme == QLatin1String("omarchy-mail")) {   // "··· Show quoted text": the Renderer's own link
        if (const QString path = url.path(); path.startsWith(QLatin1String("quotes/"))) {
            m_expanded.insert(path.mid(7).toInt());
            layout();
        }
        return;
    }
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        launch::open(QString::fromUtf8(url.toEncoded()));
    else if (scheme == QLatin1String("mailto"))
        Q_EMIT composeTo(url);
    // anything else (javascript:, file:, data:, …) is ignored
}

} // namespace om
