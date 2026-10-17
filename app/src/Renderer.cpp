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
// past the caps is shown as its text. On the way it notes the text and whether there is a picture.
static bool tooDeep(const QByteArray &html, QString *text, bool *picture)
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

void Renderer::render(const RenderJob &job)
{
    m_latest = job.generation;
    m_pool.start([this, job] {
        if (job.generation != m_latest)
            return;
        auto holder = std::make_shared<Holder>();
        Rendered r;
        r.generation = job.generation;
        r.doc = std::shared_ptr<DocumentContainer>(holder, &holder->doc);
        DocumentContainer &doc = holder->doc;

        DocumentContainerContext ctx;
        ctx.setMasterStyleSheet(QString::fromUtf8(litehtml::master_css));
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
        doc.setDataCallback([&job, &r](const QUrl &url) { return loadData(url, job, &r.wanted); });
        QByteArray html = job.html.toUtf8();
        QString text;
        bool picture = false;
        if (tooDeep(html, &text, &picture))
            html = (QStringLiteral("<pre style=\"white-space:pre-wrap\">") + text.toHtmlEscaped()
                    + QStringLiteral("</pre>")).toUtf8();
        // spaces and invisible marks (&zwnj;, U+200B) only: nothing to read
        r.blank = !picture && std::all_of(text.cbegin(), text.cend(), [](QChar c) {
            return c.isSpace() || c.category() == QChar::Other_Format;
        });
        doc.setDocument(html, &ctx);
        doc.render(job.width, job.viewportHeight);
        doc.setDataCallback([](const QUrl &) { return QByteArray(); }); // job and r are gone after this task

        r.size = QSize(doc.documentWidth(), doc.documentHeight());
        QImage probe(qMax(1, job.width), qBound(1, r.size.height(), 1500), QImage::Format_RGB32);
        probe.fill(Qt::white);
        QPainter p(&probe);
        doc.draw(&p, probe.rect());
        p.end();
        r.dark = recolor::isDarkLetter(probe);
        r.links = doc.links();

        holder.reset();
        // the document dies on the main thread: the last reference leaves this one by move
        QMetaObject::invokeMethod(
            this,
            [this, res = std::move(r)] {
                if (res.generation == m_latest)
                    Q_EMIT rendered(res);
            },
            Qt::QueuedConnection);
    });
}

} // namespace om
