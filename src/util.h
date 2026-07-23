// Small formatting / color helpers shared across the UI.
#pragma once

#include <gdkmm/rgba.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace util {

// Human-readable byte size. iec=true -> base-1024 (bytes/KiB/MiB/GiB/TiB),
// iec=false -> base-1000 (bytes/kB/MB/GB/TB), matching GNOME System Monitor's
// mix (memory/swap use SI "GB"; network/disk totals use IEC "GiB").
inline std::string format_size(double bytes, bool iec = true) {
    const double base = iec ? 1024.0 : 1000.0;
    static const char* iec_u[] = {"bytes", "KiB", "MiB", "GiB", "TiB", "PiB"};
    static const char* si_u[]  = {"bytes", "kB",  "MB",  "GB",  "TB",  "PB"};
    const char** u = iec ? iec_u : si_u;

    if (bytes < base) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f %s", bytes, u[0]);
        return buf;
    }
    int i = 0;
    double v = bytes;
    while (v >= base && i < 5) { v /= base; ++i; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
    return buf;
}

// A byte rate, e.g. "297.9 KiB/s" or "649 bytes/s".
inline std::string format_rate(double bytes_per_sec, bool iec = true) {
    return format_size(bytes_per_sec, iec) + "/s";
}

// Round a positive value up to a visually pleasant axis maximum.
inline double nice_ceil(double x) {
    if (x <= 0) return 1.0;
    double e = std::floor(std::log10(x));
    double base = std::pow(10.0, e);
    double f = x / base;  // 1 .. <10
    double nf;
    if (f <= 1.0)      nf = 1.0;
    else if (f <= 2.0) nf = 2.0;
    else if (f <= 3.0) nf = 3.0;
    else if (f <= 4.0) nf = 4.0;
    else if (f <= 5.0) nf = 5.0;
    else if (f <= 6.0) nf = 6.0;
    else if (f <= 8.0) nf = 8.0;
    else               nf = 10.0;
    return nf * base;
}

inline std::string to_hex(const Gdk::RGBA& c) {
    auto q = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    char b[8];
    std::snprintf(b, sizeof(b), "#%02x%02x%02x", q(c.get_red()), q(c.get_green()),
                  q(c.get_blue()));
    return b;
}

inline Gdk::RGBA rgb(double r, double g, double b, double a = 1.0) {
    Gdk::RGBA c;
    c.set_rgba(r, g, b, a);
    return c;
}

inline Gdk::RGBA rgb8(int r, int g, int b, double a = 1.0) {
    return rgb(r / 255.0, g / 255.0, b / 255.0, a);
}

inline Gdk::RGBA hsv(double h, double s, double v, double a = 1.0) {
    h = std::fmod(h, 360.0);
    if (h < 0) h += 360.0;
    double c = v * s;
    double x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1));
    double m = v - c;
    double r = 0, g = 0, b = 0;
    if      (h < 60)  { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    return rgb(r + m, g + m, b + m, a);
}

// Brand accent colors, in chart-series order. Amber is the primary data
// accent; Slate Blue / Sage / Plum extend the series; Sand is a neutral fill;
// Coral is reserved for alerts, so it sits last.
namespace accent {
inline Gdk::RGBA amber()      { return rgb8(0xE0, 0xA2, 0x4B); }
inline Gdk::RGBA slate_blue() { return rgb8(0x4E, 0x7C, 0xA1); }
inline Gdk::RGBA sage()       { return rgb8(0x8A, 0xA6, 0x7E); }
inline Gdk::RGBA plum()       { return rgb8(0x7E, 0x66, 0x99); }
inline Gdk::RGBA sand()       { return rgb8(0xC9, 0xB8, 0x96); }
inline Gdk::RGBA coral()      { return rgb8(0xD9, 0x69, 0x4F); }
}  // namespace accent

// Brand neutrals for chart chrome (backgrounds / grid / secondary text).
namespace neutral {
inline Gdk::RGBA white()      { return rgb8(0xFF, 0xFF, 0xFF); }
inline Gdk::RGBA mist()       { return rgb8(0xEA, 0xF1, 0xF0); }
inline Gdk::RGBA slate_gray() { return rgb8(0x6B, 0x76, 0x76); }
inline Gdk::RGBA ink()        { return rgb8(0x1F, 0x2A, 0x2A); }
}  // namespace neutral

// n colors for the per-device traces/swatches, drawn only from the brand
// accent colors (cycled if there are more devices than accents).
inline std::vector<Gdk::RGBA> make_palette(int n) {
    static const Gdk::RGBA base[] = {
        accent::amber(), accent::slate_blue(), accent::sage(),
        accent::plum(),  accent::sand(),       accent::coral(),
    };
    const int m = static_cast<int>(sizeof(base) / sizeof(base[0]));
    std::vector<Gdk::RGBA> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) out.push_back(base[i % m]);
    return out;
}

}  // namespace util
