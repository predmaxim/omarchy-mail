#include "Highlight.h"

#include <QRegularExpression>

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

// The one HTML walker: f(chunk, isText) -> replacement, for every text run and every markup chunk
// (tag, comment, whole style/script/title element).
template<class F>
static QString walk(const QString &h, F f)
{
    QString out;
    const qsizetype n = h.size();
    qsizetype i = 0;
    while (i < n) {
        qsizetype end;
        if (h[i] != QLatin1Char('<')) {
            end = h.indexOf(QLatin1Char('<'), i);
            end = end < 0 ? n : end;
            out += f(QStringView(h).mid(i, end - i), true);
            i = end;
            continue;
        }
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
        out += f(QStringView(h).mid(i, end - i), false);
        i = end;
    }
    return out;
}

QString html(const QString &h, const QStringList &ws, const QColor &mark)
{
    if (ws.isEmpty())
        return h;
    const QString open = QStringLiteral("<span style=\"background-color:rgba(%1,%2,%3,0.4)\">")
                             .arg(mark.red()).arg(mark.green()).arg(mark.blue());
    return walk(h, [&](QStringView t, bool text) {
        if (!text)
            return t.toString();
        QString out;
        QChar prev = QLatin1Char(' ');   // a tag ends a word: `</td><td>Ozon`
        const QString s = t.toString();
        const qsizetype n = s.size();
        qsizetype i = 0;
        while (i < n) {
            if (s[i] == QLatin1Char('&')) {
                qsizetype end = i + 1;
                while (end < n && end - i < 12 && (s[end].isLetterOrNumber() || s[end] == QLatin1Char('#')))
                    ++end;
                if (end < n && s[end] == QLatin1Char(';')) {
                    out += QStringView(s).mid(i, end + 1 - i);
                    i = end + 1;
                    prev = QLatin1Char(';');
                    continue;
                }
            }
            if (wordStart(prev))
                if (const qsizetype m = matchAt(s, i, ws)) {
                    out += open + QStringView(s).mid(i, m) + QStringLiteral("</span>");
                    prev = s[i + m - 1];
                    i += m;
                    continue;
                }
            out += s[i];
            prev = s[i++];
        }
        return out;
    });
}

// Bare http(s)://, www. and e-mail addresses in a text run. URL goes first: `https://user@host` is one URL.
static QString linkText(const QString &s)
{
    static const QRegularExpression re(QStringLiteral(
        "(?<![\\p{L}\\p{N}])(?:https?://|www\\.)[^\\s<>\"]+"
        "|[\\p{L}\\p{N}._%+-]+@[\\p{L}\\p{N}-]+(?:\\.[\\p{L}\\p{N}-]+)*\\.\\p{L}{2,}"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype done = 0;
    for (auto it = re.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        QString a = m.captured();
        const bool mail = a.contains(QLatin1Char('@')) && !a.startsWith(u"http", Qt::CaseInsensitive)
            && !a.startsWith(u"www.", Qt::CaseInsensitive);
        if (!mail) {
            for (const QString &e : {u"&lt;"_s, u"&gt;"_s, u"&quot;"_s})   // escaped <url> or "url"
                if (const qsizetype k = a.indexOf(e); k >= 0)
                    a.truncate(k);
            for (bool again = true; again && !a.isEmpty();) {
                again = true;
                const QChar c = a.back();
                if (c == QLatin1Char(')')) {
                    again = a.count(QLatin1Char('(')) < a.count(QLatin1Char(')'));
                } else if (c == QLatin1Char(';')) {
                    static const QRegularExpression ent(QStringLiteral("&#?\\w+;$"));
                    again = !ent.match(a).hasMatch();
                } else {
                    again = QStringView(u".,:!?\"'\u00bb\u201d\u2019\u203a").contains(c);
                }
                if (again)
                    a.chop(1);
            }
            if (a.isEmpty())
                continue;
        }
        const QString href = mail ? "mailto:" + a : a.startsWith(u"www.", Qt::CaseInsensitive) ? "https://" + a : a;
        out += s.mid(done, m.capturedStart() - done) + u"<a href=\""_s + href + u"\">"_s + a + u"</a>"_s;
        done = m.capturedStart() + a.size();
    }
    return out + s.mid(done);
}

QString links(const QString &h)
{
    int inLink = 0;
    return walk(h, [&](QStringView t, bool text) {
        if (!text) {
            const QStringView name = t.mid(1);
            if (name.startsWith(u"a", Qt::CaseInsensitive) && name.size() > 1 && !name[1].isLetterOrNumber())
                ++inLink;
            else if (name.startsWith(u"/a", Qt::CaseInsensitive) && name.size() > 2 && !name[2].isLetterOrNumber())
                inLink = qMax(0, inLink - 1);
            return t.toString();
        }
        return inLink ? t.toString() : linkText(t.toString());
    });
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
