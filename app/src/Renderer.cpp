#include "Renderer.h"

#include "Images.h"
#include "Recolor.h"

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <gumbo.h>
#include <litehtml/master_css.h>
#include <algorithm>
#include <vector>

namespace om {

static QByteArray readAll(const QString &path)
{
    QFile f(path);
    return !path.isEmpty() && f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray loadData(const QUrl &url, const RenderJob &job, QStringList *wanted)
{
    const QString scheme = url.scheme();
    if (scheme == QLatin1String("cid"))
        return readAll(job.inlinePaths.value(url.path(QUrl::FullyDecoded)));
    if (scheme == QLatin1String("data")) {
        const QByteArray s = url.toEncoded();
        const qsizetype comma = s.indexOf(',');
        const QByteArray meta = s.mid(5, comma - 5).toLower();
        if (comma < 0 || !meta.startsWith("image/"))
            return {};
        const QByteArray body = QByteArray::fromPercentEncoding(s.mid(comma + 1));
        return meta.endsWith(";base64") ? QByteArray::fromBase64(body) : body;
    }
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
        const QByteArray data = readAll(Images::pathFor(job.imageDir, url));
        const QString s = QString::fromUtf8(url.toEncoded());
        if (data.isEmpty() && wanted && !wanted->contains(s))
            wanted->append(s);
        return data;
    }
    return {};
}

// Something to see besides text: a picture, media, a background.
static bool visual(const GumboElement &e)
{
    switch (e.tag) {
    case GUMBO_TAG_IMG: case GUMBO_TAG_SVG: case GUMBO_TAG_PICTURE: case GUMBO_TAG_VIDEO:
    case GUMBO_TAG_CANVAS: case GUMBO_TAG_OBJECT: case GUMBO_TAG_EMBED:
        return true;
    default:
        break;
    }
    for (unsigned i = 0; i < e.attributes.length; ++i) {
        const auto *a = static_cast<const GumboAttribute *>(e.attributes.data[i]);
        const QByteArray name(a->name), value = QByteArray(a->value).toLower();
        if (name == "background" || name == "bgcolor" || (name == "style" && value.contains("url("))
            || (e.tag == GUMBO_TAG_INPUT && name == "type" && value == "image"))
            return true;
    }
    return false;
}

// litehtml recurses per nesting level (parse, layout, draw, and the tree's destruction on the main
// thread) and lays nested tables out in ~3^n time: 16000 nested divs overflow 8 MiB, 14 nested
// tables take 46 s (measured 2026-10-17). gumbo, its own parser, measures the tree first; a letter
// past the caps is shown as its text. On the way it notes the text, whether there is a picture and quotes.
static bool tooDeep(const QByteArray &html, QString *text, bool *picture, bool *quote)
{
    constexpr int maxDepth = 1000, maxTables = 12;
    GumboOutput *out = gumbo_parse_with_options(&kGumboDefaultOptions, html.constData(), size_t(html.size()));
    struct Item
    {
        const GumboNode *node;
        int depth, tables;
    };
    std::vector<Item> todo{{out->root, 0, 0}};
    bool deep = false;
    while (!todo.empty()) {
        const Item it = todo.back();
        todo.pop_back();
        if (it.node->type == GUMBO_NODE_TEXT || it.node->type == GUMBO_NODE_WHITESPACE
            || it.node->type == GUMBO_NODE_CDATA) {
            *text += QString::fromUtf8(it.node->v.text.text);
            continue;
        }
        const GumboElement &e = it.node->v.element;
        if (it.node->type != GUMBO_NODE_ELEMENT || e.tag == GUMBO_TAG_STYLE || e.tag == GUMBO_TAG_SCRIPT
            || e.tag == GUMBO_TAG_TITLE)
            continue;
        *picture = *picture || visual(e);
        *quote = *quote || e.tag == GUMBO_TAG_BLOCKQUOTE;
        if (e.tag == GUMBO_TAG_BR)
            *text += QLatin1Char('\n');
        const int tables = it.tables + (e.tag == GUMBO_TAG_TABLE);
        deep = deep || it.depth > maxDepth || tables > maxTables;
        for (unsigned i = e.children.length; i-- > 0;)
            todo.push_back({static_cast<const GumboNode *>(e.children.data[i]), it.depth + 1, tables});
    }
    gumbo_destroy_output(&kGumboDefaultOptions, out);
    return deep;
}

// Our user-agent sheet after litehtml's, like Geary's (a letter's own CSS wins): roomier lines, <pre> that
// wraps, quotes with a bar on the left only. The bar and the quote's text are the greys the colour filter
// turns into the theme's muted and dim, and the filter runs the other way on a dark letter.
static QString userAgentCss(const RenderJob &job, bool dark)
{
    const auto grey = [&](const QColor &c) { return recolor::unmap(c, job.background, job.foreground, dark).name(); };
    return QStringLiteral("body{margin:12px;line-height:1.7}h1,h2,h3,h4,h5,h6{line-height:1.2}table{font-size:inherit}"
                          "pre{white-space:pre-wrap}"
                          "blockquote{margin:.3em 0 .3em 16px;padding:0 0 0 8px;border-left:3px solid %1;color:%2}")
        .arg(grey(job.muted), grey(job.dim));
}

Renderer::Renderer(QObject *parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(1);
}

Renderer::~Renderer()
{
    m_pool.clear();
    m_pool.waitForDone();
}

// The paint device lives as long as the document that measures with it.
struct Holder
{
    QImage device{10, 10, QImage::Format_RGB32};
    DocumentContainer doc;
};

// Lays html out at the job's width; images come from loadData, the missing ones go to `wanted`.
static std::shared_ptr<Holder> layOut(const QByteArray &html, const QString &css, const RenderJob &job,
                                      QStringList *wanted)
{
    auto holder = std::make_shared<Holder>();
    DocumentContainer &doc = holder->doc;
    DocumentContainerContext ctx;
    ctx.setMasterStyleSheet(QString::fromUtf8(litehtml::master_css) + css);
    doc.setPaintDevice(&holder->device);
    QFont font = job.font;
    if (font.pixelSize() > 0) // qlitehtml hands pointSize() to litehtml as pixels
        font.setPointSize(font.pixelSize());
    doc.setDefaultFont(font);
    doc.setPaletteCallback([sel = job.selection] {
        QPalette p;
        p.setColor(QPalette::Highlight, sel);
        return p;
    });
    doc.setDataCallback([&job, wanted](const QUrl &url) { return loadData(url, job, wanted); });
    doc.render(job.width, job.viewportHeight);   // no document yet: sets the viewport its @media rules see
    doc.setDocument(html, &ctx);
    doc.render(job.width, job.viewportHeight);
    doc.setDataCallback([](const QUrl &) { return QByteArray(); }); // job and wanted are gone after the task
    return holder;
}

static bool isDark(DocumentContainer &doc, int width)
{
    QImage probe(qMax(1, width), qBound(1, doc.documentHeight(), 1500), QImage::Format_RGB32);
    probe.fill(Qt::white);
    QPainter p(&probe);
    doc.draw(&p, probe.rect());
    p.end();
    return recolor::isDarkLetter(probe);
}

void Renderer::render(const RenderJob &job)
{
    m_latest = job.generation;
    m_pool.start([this, job] {
        if (job.generation != m_latest)
            return;
        Rendered r;
        r.generation = job.generation;
        QByteArray html = job.html.toUtf8();
        QString text;
        bool picture = false, quote = false;
        if (tooDeep(html, &text, &picture, &quote)) {
            html = (QStringLiteral("<pre style=\"white-space:pre-wrap\">") + text.toHtmlEscaped()
                    + QStringLiteral("</pre>")).toUtf8();
            quote = false;
        }
        // spaces and invisible marks (&zwnj;, U+200B) only: nothing to read
        r.blank = !picture && std::all_of(text.cbegin(), text.cend(), [](QChar c) {
            return c.isSpace() || c.category() == QChar::Other_Format;
        });
        auto holder = layOut(html, userAgentCss(job, false), job, &r.wanted);
        r.dark = isDark(holder->doc, job.width);
        std::shared_ptr<Holder> dropped;
        if (r.dark && quote)   // the quotes' greys were for a light letter
            dropped = std::exchange(holder, layOut(html, userAgentCss(job, true), job, &r.wanted));
        r.size = QSize(holder->doc.documentWidth(), holder->doc.documentHeight());
        r.links = holder->doc.links();
        r.doc = std::shared_ptr<DocumentContainer>(holder, &holder->doc);

        holder.reset();
        // documents die on the main thread: the last references leave this one by move
        QMetaObject::invokeMethod(
            this,
            [this, res = std::move(r), dropped = std::move(dropped)] {
                if (res.generation == m_latest)
                    Q_EMIT rendered(res);
            },
            Qt::QueuedConnection);
    });
}

} // namespace om
