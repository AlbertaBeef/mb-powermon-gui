// A scrolling multi-series line graph, drawn with Cairo, styled after the
// GNOME System Monitor resource graphs: faint grid, right-hand scale labels,
// time labels along the bottom, newest sample at the right edge.
#pragma once

#include <gtkmm/drawingarea.h>
#include <gdkmm/rgba.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

class GraphArea : public Gtk::DrawingArea {
public:
    // history: max number of points kept per series (also the x-axis extent).
    // span_seconds: wall-clock width of the plot, for the time labels.
    GraphArea(int history, int span_seconds);

    void set_series(const std::vector<Gdk::RGBA>& colors);
    void set_series_color(int i, const Gdk::RGBA& c);
    // Hide a series without discarding its history: it stops being drawn AND
    // stops counting toward the axis range, so the visible traces get the whole
    // plot. Filtering at draw time rather than at push() is what makes the
    // change retroactive — masking new samples alone would leave the last ten
    // minutes of hidden traces on screen.
    void set_series_visible(int i, bool on);
    void set_all_series_visible(bool on);
    int series_count() const { return static_cast<int>(series_.size()); }

    // Push one new sample per series (same order as set_series). Values are in
    // data units; for percent graphs pass 0..1.
    void push(const std::vector<double>& values);

    // How the axis top responds to the data. Every mode below (percent,
    // fixed-max, auto) supplies a *baseline* top; RangeMode decides what
    // happens when the readings approach or fall short of it.
    enum class RangeMode {
        Fixed,    // baseline only — the original behaviour; data above it clips
        Max,      // baseline, re-topped at 10 % above the peak once reached
        Dynamic,  // both ends track the data, so the plot is always filled
    };
    void set_range_mode(RangeMode m) { range_mode_ = m; queue_draw(); }
    RangeMode range_mode() const { return range_mode_; }

    // Percent mode: 0..1 axis, labels "50 %/75 %/100 %".
    void set_percent_mode(bool on) { percent_mode_ = on; }
    // Fixed-max mode: the baseline axis top (e.g. 100 for °C); labels via the
    // value formatter. Whether a reading above it grows the axis is up to
    // RangeMode — it is a hard cap only in RangeMode::Fixed.
    void set_fixed_max(double v) { fixed_max_ = v; }
    // Auto mode (default when neither percent nor fixed): axis max tracks the
    // data peak; labels via the value formatter.
    void set_value_formatter(std::function<std::string(double)> f) {
        value_formatter_ = std::move(f);
    }
    // Floor for the auto axis, so an idle graph doesn't scale itself to noise.
    void set_min_axis_max(double v) { min_axis_max_ = v; }
    // Fill the area under each trace with a translucent wash (nice for the
    // single/low-series memory & i/o graphs; left off for the 128 CPU lines).
    void set_fill(bool on) { fill_ = on; }

    void reset();

private:
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    // The visible value range. `lo` is 0 in every mode but Dynamic, where it
    // tracks the trough so a narrow band of readings fills the plot height.
    void axis_range(double& lo, double& hi) const;

    int history_;
    int span_seconds_;
    bool percent_mode_ = true;
    bool fill_ = false;
    double fixed_max_ = 0.0;
    RangeMode range_mode_ = RangeMode::Max;
    double min_axis_max_ = 1.0;
    std::function<std::string(double)> value_formatter_;

    std::vector<std::deque<double>> series_;
    std::vector<Gdk::RGBA> colors_;
    std::vector<bool> visible_;   // parallel to series_; all true by default
};
