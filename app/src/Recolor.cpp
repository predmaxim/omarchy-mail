#include "Recolor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace om::recolor {

static QColor mix(const QColor &a, const QColor &b, float t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

// WCAG 2 relative luminance
static double luminance(const QColor &c)
{
    const auto l = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * l(c.redF()) + 0.7152 * l(c.greenF()) + 0.0722 * l(c.blueF());
}

// The theme's grey at t (0 = bg, 1 = fg) blended with c's hue, as light as that grey, by c's saturation
static QColor at(const QColor &c, const QColor &bg, const QColor &fg, float t)
{
    const float s = c.hslSaturationF();
    const QColor grey = mix(bg, fg, t);
    const float l2 = bg.lightnessF() + (fg.lightnessF() - bg.lightnessF()) * t;
    const QColor hue = c.hslHue() < 0 ? grey : QColor::fromHslF(c.hslHueF(), s, l2);
    return mix(grey, hue, s);
}

QColor map(const QColor &c, const QColor &bg, const QColor &fg, bool darkLetter)
{
    // c's place between black and white in log luminance, reflected into bg..fg: every contrast C becomes C^k
    const double yb = luminance(bg) + 0.05, yf = luminance(fg) + 0.05;
    const double pos = std::log((luminance(c) + 0.05) / 0.05) / std::log(21.0);
    const double target = yb * std::pow(yf / yb, darkLetter ? pos : 1 - pos) - 0.05;
    float lo = 0, hi = 1;
    for (int i = 0; i < 24; ++i) {
        const float mid = (lo + hi) / 2;
        ((luminance(at(c, bg, fg, mid)) < target) == (yf > yb) ? lo : hi) = mid;
    }
    QColor out = at(c, bg, fg, (lo + hi) / 2);
    out.setAlpha(c.alpha());
    return out;
}

std::function<QColor(const QColor &)> filter(QColor bg, QColor fg, bool darkLetter)
{
    return [=](const QColor &c) { return map(c, bg, fg, darkLetter); };
}

QColor unmap(const QColor &target, const QColor &bg, const QColor &fg, bool darkLetter)
{
    // the source luminance whose map() is the target's: map()'s reflection run backwards; past the range, its end
    const double yb = luminance(bg) + 0.05, yf = luminance(fg) + 0.05;
    const double e = yf == yb ? 0.5 : std::clamp(std::log((luminance(target) + 0.05) / yb) / std::log(yf / yb), 0.0, 1.0);
    const double Y = 0.05 * std::pow(21.0, darkLetter ? e : 1 - e) - 0.05;

    // the hue and saturation s whose at() is the target: P is its coloured part, of lightness l2(t) and saturation s
    using Rgb = std::array<double, 3>;
    const Rgb T{target.redF(), target.greenF(), target.blueF()}, B{bg.redF(), bg.greenF(), bg.blueF()},
        F{fg.redF(), fg.greenF(), fg.blueF()};
    const double Lb = bg.lightnessF(), Lf = fg.lightnessF();
    const auto P = [&](double s, double t) {
        Rgb p;
        for (int i = 0; i < 3; ++i)
            p[i] = (T[i] - (1 - s) * (B[i] + (F[i] - B[i]) * t)) / s;
        return p;
    };
    const auto lightness = [](const Rgb &p) { return (*std::ranges::max_element(p) + *std::ranges::min_element(p)) / 2; };
    const auto saturation = [&](const Rgb &p) {
        const double hi = *std::ranges::max_element(p), lo = *std::ranges::min_element(p), l = lightness(p);
        if (lo < 0 || hi > 1)
            return 2.0;   // not a colour (a channel past 0..1): more saturated than any
        return hi == lo ? 0.0 : (hi - lo) / (1 - std::abs(2 * l - 1));
    };
    const auto tOf = [&](double s) {   // where P is as light as the grey at t
        double lo = 0, hi = 1;
        for (int i = 0; i < 24; ++i) {
            const double mid = (lo + hi) / 2;
            ((lightness(P(s, mid)) > Lb + (Lf - Lb) * mid) == (Lf > Lb) ? lo : hi) = mid;
        }
        return (lo + hi) / 2;
    };
    double lo = 1e-4, hi = 1;
    for (int i = 0; i < 24; ++i) {
        const double mid = (lo + hi) / 2;
        (saturation(P(mid, tOf(mid))) > mid ? lo : hi) = mid;
    }
    const double s = (lo + hi) / 2;
    const Rgb p = P(s, tOf(s));
    const auto unit = [](double v) { return float(std::clamp(v, 0.0, 1.0)); };
    const float h = QColor::fromRgbF(unit(p[0]), unit(p[1]), unit(p[2])).hslHueF();
    const float sat = h < 0 || s < 1e-3 ? 0 : s;   // a grey has no hue to keep

    // that hue and saturation at the source luminance
    float l0 = 0, l1 = 1;
    for (int i = 0; i < 24; ++i) {
        const float mid = (l0 + l1) / 2;
        (luminance(QColor::fromHslF(qMax(0.f, h), sat, mid)) < Y ? l0 : l1) = mid;
    }
    const QColor c0 = QColor::fromHslF(qMax(0.f, h), sat, (l0 + l1) / 2);

    // litehtml keeps colours in bytes, and rounding a dark saturated colour moves its map() far: step to the best
    // byte neighbour until none maps nearer (±2 would pull targets past the range toward other hues).
    // shortcut: a local search, so hackerman's accent in a light letter lands 7 off where a byte 4 off exists;
    // search along the saturation if a theme's accent or greys visibly drift.
    QColor best(c0.red(), c0.green(), c0.blue()), from;
    double bestD = 1e9;
    while (from != best) {
        from = best;
        for (int dr = -1; dr <= 1; ++dr)
            for (int dg = -1; dg <= 1; ++dg)
                for (int db = -1; db <= 1; ++db) {
                    const QColor u(std::clamp(from.red() + dr, 0, 255), std::clamp(from.green() + dg, 0, 255),
                                   std::clamp(from.blue() + db, 0, 255));
                    const QColor o = map(u, bg, fg, darkLetter);
                    const double d = std::abs(o.redF() - T[0]) + std::abs(o.greenF() - T[1]) + std::abs(o.blueF() - T[2]);
                    if (d < bestD)
                        bestD = d, best = u;
                }
    }
    return best;
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
