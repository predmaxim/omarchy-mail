#include "Recolor.h"

#include <algorithm>
#include <vector>

namespace om::recolor {

static QColor mix(const QColor &a, const QColor &b, float t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

QColor map(const QColor &c, const QColor &bg, const QColor &fg, bool darkLetter)
{
    const float L = c.lightnessF(), s = c.hslSaturationF();
    const float t = darkLetter ? L : 1 - L;
    const QColor grey = mix(bg, fg, t);
    const float l2 = bg.lightnessF() + (fg.lightnessF() - bg.lightnessF()) * t;
    const QColor hue = c.hslHue() < 0 ? grey : QColor::fromHslF(c.hslHueF(), s, l2);
    QColor out = mix(grey, hue, s);
    out.setAlpha(c.alpha());
    return out;
}

std::function<QColor(const QColor &)> filter(QColor bg, QColor fg, bool darkLetter)
{
    return [=](const QColor &c) { return map(c, bg, fg, darkLetter); };
}

bool isDarkLetter(const QImage &probe)
{
    const QImage img = probe.convertToFormat(QImage::Format_ARGB32);
    std::vector<float> ls;
    for (int y = 0; y < img.height(); y += 4)
        for (int x = 0; x < img.width(); x += 4)
            ls.push_back(QColor::fromRgb(img.pixel(x, y)).lightnessF());
    if (ls.empty())
        return false;
    auto mid = ls.begin() + ls.size() / 2;
    std::nth_element(ls.begin(), mid, ls.end());
    return *mid < 0.5f;
}

} // namespace om::recolor
