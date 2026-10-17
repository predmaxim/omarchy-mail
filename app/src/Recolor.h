#pragma once
#include <QColor>
#include <QImage>
#include <functional>

namespace om::recolor {

// Maps a letter colour into the theme range; darkLetter = the letter already has a dark background.
QColor map(const QColor &c, const QColor &bg, const QColor &fg, bool darkLetter);
std::function<QColor(const QColor &)> filter(QColor bg, QColor fg, bool darkLetter);
// The colour map() turns into about `target` (as light as it within bg..fg, of its hue): our own colours in a letter.
QColor unmap(const QColor &target, const QColor &bg, const QColor &fg, bool darkLetter);
// Median HSL lightness of every 4th pixel (both axes) < 0.5.
bool isDarkLetter(const QImage &probe);

} // namespace om::recolor
