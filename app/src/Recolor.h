#pragma once
#include <QColor>
#include <QImage>
#include <functional>

namespace om::recolor {

// Maps a letter colour by WCAG luminance, reflected in log space into the theme's range bg..fg, and keeps its hue:
// any contrast C between two mapped colours becomes C^k, k = ln contrast(bg, fg) / ln 21.
// darkLetter = the letter already has a dark background.
QColor map(const QColor &c, const QColor &bg, const QColor &fg, bool darkLetter);
// map() remembered per colour (rgba); not locked: one painter at a time, and every copy shares the cache.
std::function<QColor(const QColor &)> filter(QColor bg, QColor fg, bool darkLetter);
// An 8-bit colour (litehtml keeps colours in bytes) whose map() is near target, no byte a step of ±1 per channel
// away maps nearer: our own colours in a letter. A search from the exact inverse, so not always the nearest of all.
QColor unmap(const QColor &target, const QColor &bg, const QColor &fg, bool darkLetter);
// Median HSL lightness of every 4th pixel (both axes) < 0.5.
bool isDarkLetter(const QImage &probe);

} // namespace om::recolor
