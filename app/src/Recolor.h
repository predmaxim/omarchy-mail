#pragma once
#include <QColor>
#include <QImage>
#include <functional>

namespace om::recolor {

// Maps a letter colour by WCAG luminance, reflected in log space into the theme's range bg..fg, and keeps its hue:
// any contrast C between two mapped colours becomes C^k, k = ln contrast(bg, fg) / ln 21.
// darkLetter = the letter already has a dark background.
QColor map(const QColor &c, const QColor &bg, const QColor &fg, bool darkLetter);
std::function<QColor(const QColor &)> filter(QColor bg, QColor fg, bool darkLetter);
// The 8-bit colour whose map() is nearest target (litehtml keeps colours in bytes): our own colours in a letter.
QColor unmap(const QColor &target, const QColor &bg, const QColor &fg, bool darkLetter);
// Median HSL lightness of every 4th pixel (both axes) < 0.5.
bool isDarkLetter(const QImage &probe);

} // namespace om::recolor
