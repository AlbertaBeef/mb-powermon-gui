#include "GraphArea.h"

#include "util.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kMarginTop = 8;
constexpr int kMarginBottom = 20;  // room for the time labels
constexpr int kMarginLeft = 6;
constexpr int kMarginRight = 54;   // room for the scale labels
// Air left above the highest sample once the data outgrows a graph's baseline
// top. 1.10 = "10 % above the max".
constexpr double kHeadroom = 1.10;
}  // namespace

GraphArea::GraphArea(int history, int span_seconds)
    : history_(history), span_seconds_(span_seconds) {
    set_content_height(90);  // minimum; grows to fill via vexpand
    set_hexpand(true);
    set_vexpand(true);
    set_draw_func(sigc::mem_fun(*this, &GraphArea::draw));
}

void GraphArea::set_series(const std::vector<Gdk::RGBA>& colors) {
    colors_ = colors;
    series_.assign(colors.size(), std::deque<double>());
    visible_.assign(colors.size(), true);
}

void GraphArea::set_series_visible(int i, bool on) {
    if (i < 0 || i >= static_cast<int>(visible_.size())) return;
    if (visible_[i] == on) return;
    visible_[i] = on;
    queue_draw();
}

void GraphArea::set_all_series_visible(bool on) {
    bool changed = false;
    for (size_t i = 0; i < visible_.size(); ++i)
        if (visible_[i] != on) { visible_[i] = on; changed = true; }
    if (changed) queue_draw();
}

void GraphArea::set_series_color(int i, const Gdk::RGBA& c) {
    if (i >= 0 && i < static_cast<int>(colors_.size())) colors_[i] = c;
}

void GraphArea::push(const std::vector<double>& values) {
    if (series_.size() != values.size()) return;
    for (size_t i = 0; i < series_.size(); ++i) {
        series_[i].push_back(values[i]);
        while (static_cast<int>(series_[i].size()) > history_) series_[i].pop_front();
    }
    queue_draw();
}

void GraphArea::reset() {
    for (auto& s : series_) s.clear();
    queue_draw();
}

// The visible value range.
//
// The top: every mode has a *baseline* — 100 % for percent, `fixed_max_` for the
// temperature axis, the floor for an auto axis — and `range_mode_` decides how
// the data moves it: not at all (Fixed), upward only once the data reaches it
// (Max), or continuously (Dynamic).
//
// The bottom is 0 everywhere except Dynamic, which tracks the trough too. That
// is the difference that makes Dynamic worth having: four cards sitting between
// 37 °C and 42 °C occupy five pixels of a 0..100 axis, and the whole plot height
// on a 36.5..42.5 one.
//
// `fixed_max_` is therefore a *starting* top, not a cap: a die past 100 °C used
// to draw as a flat line pinned to the top edge, indistinguishable from one
// sitting exactly at 100. Rounding the expanded top to a "nice" number is
// deliberately not done — it would quantize 1370 up to 2000 and leave the trace
// in the bottom half of the plot, which is the readability problem the headroom
// rule exists to avoid.
void GraphArea::axis_range(double& lo, double& hi) const {
    // Never zero-width: draw() divides by (hi - lo), and a caller is free to
    // pass a 0 floor to set_min_axis_max().
    const double base = std::max(percent_mode_      ? 1.0
                                 : fixed_max_ > 0.0 ? fixed_max_
                                                    : min_axis_max_,
                                 1e-9);
    double peak = 0.0, trough = 0.0;
    bool any = false;
    for (size_t si = 0; si < series_.size(); ++si) {
        if (si < visible_.size() && !visible_[si]) continue;
        const auto& s = series_[si];
        for (double v : s) {
            if (std::isnan(v)) continue;
            if (!any) { peak = trough = v; any = true; continue; }
            peak = std::max(peak, v);
            trough = std::min(trough, v);
        }
    }

    lo = 0.0;
    switch (range_mode_) {
        case RangeMode::Fixed:
            // The baseline is the whole story: a reading above it is clipped by
            // draw()'s clamp. This is what every graph did originally.
            hi = base;
            return;
        case RangeMode::Dynamic: {
            if (!any) { hi = base; return; }
            // Margins are a share of the *span*, not of the value, so a flat
            // band gets air above and below it rather than a top-heavy axis.
            // A perfectly flat trace has no span, hence the value-relative and
            // absolute fallbacks.
            const double span = peak - trough;
            // 10 % of the span at each end. The value-relative term is a
            // fallback for a *flat* trace only — using max() of the two would
            // let it dominate every narrow band (four dies between 37 and 42 °C
            // would get a 34.9..44.1 axis and fill barely half the height,
            // which defeats the mode).
            double margin = span * (kHeadroom - 1.0);
            if (!(margin > 0.0)) margin = std::fabs(peak) * 0.05;
            // Every sample is 0 (an idle benchmark graph): there is nothing to
            // scale to, so rest at the baseline rather than on a 1e-9 axis
            // whose gridlines would all format as "0".
            if (!(margin > 0.0)) { hi = base; return; }
            hi = peak + margin;
            lo = trough - margin;
            // Data that never goes negative keeps a non-negative axis: a frame
            // rate resting at 0 should sit on the floor, not float above -137.
            if (trough >= 0.0) lo = std::max(0.0, lo);
            return;
        }
        case RangeMode::Max:
        default:
            // Baseline until the data reaches it, then 10 % of air above the
            // peak. Never shrinks below the baseline, so the axis a graph rests
            // at is stable between runs.
            hi = (any && peak > base) ? peak * kHeadroom : base;
            return;
    }
}

void GraphArea::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const double px = kMarginLeft;
    const double py = kMarginTop;
    const double pw = std::max(1.0, double(w) - kMarginLeft - kMarginRight);
    const double ph = std::max(1.0, double(h) - kMarginTop - kMarginBottom);

    auto use = [&](const Gdk::RGBA& c, double a = 1.0) {
        cr->set_source_rgba(c.get_red(), c.get_green(), c.get_blue(), a);
    };

    // Plot background — original near-white (#FAFAFA).
    cr->rectangle(px, py, pw, ph);
    cr->set_source_rgb(0.98, 0.98, 0.98);
    cr->fill();

    double axis_lo = 0.0, axis_hi = 1.0;
    axis_range(axis_lo, axis_hi);
    const double axis_span = std::max(axis_hi - axis_lo, 1e-12);

    // Horizontal grid + right-hand scale labels at 50/75/100 % of the axis.
    cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(10);
    const double fracs[] = {0.0, 0.25, 0.5, 0.75, 1.0};
    for (double fr : fracs) {
        double y = py + ph * (1.0 - fr);
        use(util::neutral::slate_gray(), 0.28);
        cr->set_line_width(1.0);
        cr->move_to(px, std::round(y) + 0.5);
        cr->line_to(px + pw, std::round(y) + 0.5);
        cr->stroke();

        std::string label;
        if (percent_mode_)
            label = std::to_string(int(std::round(fr * 100))) + " %";
        else if (value_formatter_)
            label = value_formatter_(axis_lo + axis_span * fr);
        use(util::neutral::slate_gray());
        cr->move_to(px + pw + 6, y + 3.5);
        cr->show_text(label);
    }

    // Vertical grid + time labels. The step adapts to the span: a 60 s window
    // keeps its 10 s marks, while a 10 min one gets 2 min marks instead of
    // sixty gridlines. Largest nice step that still leaves >= 5 divisions.
    int label_step = 10;
    for (int cand : {10, 15, 30, 60, 120, 300, 600}) {
        label_step = cand;
        if (span_seconds_ / cand <= 8) break;
    }
    for (int t = span_seconds_; t >= label_step; t -= label_step) {
        double x = px + pw * (1.0 - double(t) / span_seconds_);
        use(util::neutral::slate_gray(), 0.18);
        cr->set_line_width(1.0);
        cr->move_to(std::round(x) + 0.5, py);
        cr->line_to(std::round(x) + 0.5, py + ph);
        cr->stroke();

        std::string label = (t >= 60 && t % 60 == 0)
                                ? std::to_string(t / 60) + " min"
                                : std::to_string(t) + " secs";
        use(util::neutral::slate_gray());
        Cairo::TextExtents ext;
        cr->get_text_extents(label, ext);
        double lx = x - (t == span_seconds_ ? 0 : ext.width / 2);
        cr->move_to(lx, py + ph + 13);
        cr->show_text(label);
    }

    // Border around the plot.
    use(util::neutral::slate_gray(), 0.45);
    cr->set_line_width(1.0);
    cr->rectangle(px + 0.5, py + 0.5, pw, ph);
    cr->stroke();

    // Traces. Oldest sample sits (history_-1) steps left of the right edge so
    // short histories scroll in from the right.
    cr->save();
    cr->rectangle(px, py, pw, ph);
    cr->clip();
    const double step = pw / std::max(1, history_ - 1);
    for (size_t s = 0; s < series_.size(); ++s) {
        if (s < visible_.size() && !visible_[s]) continue;
        const auto& d = series_[s];
        if (d.size() < 2) continue;
        const int m = static_cast<int>(d.size());

        auto px_at = [&](int j) { return px + pw - (m - 1 - j) * step; };
        auto py_at = [&](int j) {
            double v = std::clamp((d[j] - axis_lo) / axis_span, 0.0, 1.0);
            return py + ph * (1.0 - v);
        };

        const Gdk::RGBA& c = colors_[s];

        if (fill_) {
            // Fill each contiguous (non-NaN) run down to the baseline.
            int j = 0;
            while (j < m) {
                if (std::isnan(d[j])) { ++j; continue; }
                int k = j;
                while (k < m && !std::isnan(d[k])) ++k;  // [j, k)
                cr->move_to(px_at(j), py + ph);
                for (int i = j; i < k; ++i) cr->line_to(px_at(i), py_at(i));
                cr->line_to(px_at(k - 1), py + ph);
                cr->close_path();
                cr->set_source_rgba(c.get_red(), c.get_green(), c.get_blue(),
                                    0.12);
                cr->fill();
                j = k;
            }
        }

        // Stroke each contiguous run; gaps (NaN) break the line.
        cr->set_source_rgba(c.get_red(), c.get_green(), c.get_blue(), 0.95);
        cr->set_line_width(1.25);
        bool pen_down = false;
        for (int j = 0; j < m; ++j) {
            if (std::isnan(d[j])) { pen_down = false; continue; }
            if (!pen_down) { cr->move_to(px_at(j), py_at(j)); pen_down = true; }
            else            { cr->line_to(px_at(j), py_at(j)); }
        }
        cr->stroke();
    }
    cr->restore();
}
