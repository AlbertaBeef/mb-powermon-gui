#include "GraphArea.h"

#include "util.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kMarginTop = 8;
constexpr int kMarginBottom = 20;  // room for the time labels
constexpr int kMarginLeft = 6;
constexpr int kMarginRight = 54;   // room for the scale labels
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

double GraphArea::current_axis_max() const {
    if (percent_mode_) return 1.0;
    if (fixed_max_ > 0.0) return fixed_max_;
    double peak = 0.0;
    for (const auto& s : series_)
        for (double v : s)
            if (!std::isnan(v)) peak = std::max(peak, v);
    return std::max(util::nice_ceil(peak), min_axis_max_);
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

    const double axis_max = current_axis_max();

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
            label = value_formatter_(axis_max * fr);
        use(util::neutral::slate_gray());
        cr->move_to(px + pw + 6, y + 3.5);
        cr->show_text(label);
    }

    // Vertical grid + time labels every 10 s.
    for (int t = span_seconds_; t >= 10; t -= 10) {
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
        const auto& d = series_[s];
        if (d.size() < 2) continue;
        const int m = static_cast<int>(d.size());

        auto px_at = [&](int j) { return px + pw - (m - 1 - j) * step; };
        auto py_at = [&](int j) {
            double v = std::clamp(d[j] / axis_max, 0.0, 1.0);
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
