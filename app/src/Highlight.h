#pragma once
#include <QColor>
#include <QString>
#include <QStringList>

namespace om::highlight {

QStringList words(const QString &query);
// Wraps word-start matches in text nodes only; tags, comments, style/script/title and entities stay intact.
QString html(const QString &html, const QStringList &words, const QColor &mark);
// Bare URLs, www. and e-mail addresses in text nodes (outside <a>, style/script/title, comments, attributes) become <a>.
QString links(const QString &html);
// Escapes everything (feeds Text.StyledText); matches become <b><font color>.
QString styled(const QString &plain, const QStringList &words, const QColor &mark);

} // namespace om::highlight
