#include "Highlight.h"

using namespace Qt::StringLiterals;

namespace om::highlight {

QStringList words(const QString &query)
{
    QStringList out;
    for (const QString &w : query.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        bool ok = false;
        for (QChar c : w)
            ok = ok || c.isLetterOrNumber();
        if (ok)
            out << w.toLower();
    }
    return out;
}

// Length of the longest word that is a case-insensitive prefix of s at i, 0 if none.
static qsizetype matchAt(const QString &s, qsizetype i, const QStringList &ws)
{
    qsizetype best = 0;
    for (const QString &w : ws)
        if (w.size() > best && QStringView(s).mid(i).startsWith(w, Qt::CaseInsensitive))
            best = w.size();
    return best;
}

static bool wordStart(QChar prev) { return !prev.isLetterOrNumber(); }

QString html(const QString &h, const QStringList &ws, const QColor &mark)
{
    if (ws.isEmpty())
        return h;
    const QString open = QStringLiteral("<span style=\"background-color:rgba(%1,%2,%3,0.4)\">")
                             .arg(mark.red()).arg(mark.green()).arg(mark.blue());
    QString out;
    QChar prev = QLatin1Char(' ');
    const qsizetype n = h.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = h[i];
        if (c == QLatin1Char('<')) {
            qsizetype end;
            if (QStringView(h).mid(i).startsWith(u"<!--")) {
                end = h.indexOf(QStringLiteral("-->"), i + 4);
                end = end < 0 ? n : end + 3;
            } else {
                end = i + 1;
                QChar q;
                while (end < n && (!q.isNull() || h[end] != QLatin1Char('>'))) {
                    if (!q.isNull() ? h[end] == q : (h[end] == QLatin1Char('"') || h[end] == QLatin1Char('\'')))
                        q = q.isNull() ? h[end] : QChar();
                    ++end;
                }
                end = qMin(end + 1, n);
                // raw-text elements: skip through the matching close tag
                for (const QString &tag : {u"style"_s, u"script"_s, u"title"_s}) {
                    if (QStringView(h).mid(i + 1).startsWith(tag, Qt::CaseInsensitive)
                        && i + 1 + tag.size() < n && !h[i + 1 + tag.size()].isLetterOrNumber()) {
                        const qsizetype close = h.indexOf(QStringLiteral("</") + tag, end, Qt::CaseInsensitive);
                        if (close < 0) {
                            end = n;
                        } else {
                            end = h.indexOf(QLatin1Char('>'), close);
                            end = end < 0 ? n : end + 1;
                        }
                        break;
                    }
                }
            }
            out += QStringView(h).mid(i, end - i);
            i = end;
            prev = QLatin1Char(' ');   // a tag ends a word: `</td><td>Ozon`
            continue;
        }
        if (c == QLatin1Char('&')) {
            qsizetype end = i + 1;
            while (end < n && end - i < 12 && (h[end].isLetterOrNumber() || h[end] == QLatin1Char('#')))
                ++end;
            if (end < n && h[end] == QLatin1Char(';')) {
                out += QStringView(h).mid(i, end + 1 - i);
                i = end + 1;
                prev = QLatin1Char(';');
                continue;
            }
        }
        if (wordStart(prev))
            if (const qsizetype m = matchAt(h, i, ws)) {
                out += open + QStringView(h).mid(i, m) + QStringLiteral("</span>");
                prev = h[i + m - 1];
                i += m;
                continue;
            }
        out += c;
        prev = c;
        ++i;
    }
    return out;
}

QString styled(const QString &plain, const QStringList &ws, const QColor &mark)
{
    const QString open = QStringLiteral("<b><font color=\"%1\">").arg(mark.name());
    QString out;
    QChar prev = QLatin1Char(' ');
    for (qsizetype i = 0; i < plain.size();) {
        if (wordStart(prev))
            if (const qsizetype m = matchAt(plain, i, ws)) {
                out += open + plain.mid(i, m).toHtmlEscaped() + QStringLiteral("</font></b>");
                prev = plain[i + m - 1];
                i += m;
                continue;
            }
        out += QString(plain[i]).toHtmlEscaped();
        prev = plain[i++];
    }
    return out;
}

} // namespace om::highlight
