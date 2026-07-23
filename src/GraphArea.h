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
    int series_count() const { return static_cast<int>(series_.size()); }

    // Push one new sample per series (same order as set_series). Values are in
    // data units; for percent graphs pass 0..1.
    void push(const std::vector<double>& values);

    // Percent mode: fixed 0..1 axis, labels "50 %/75 %/100 %".
    void set_percent_mode(bool on) { percent_mode_ = on; }
    // Fixed-max mode: a constant axis top (e.g. 100 for °C); labels via the
    // value formatter. Overrides auto-scaling when > 0.
    void set_fixed_max(double v) { fixed_max_ = v; }
    // Auto mode (default when neither percent nor fixed): axis max tracks the
    // data peak (nice-rounded); labels via the value formatter.
    void set_value_formatter(std::function<std::string(double)> f) {
        value_formatter_ = std::move(f);
    }
    void set_min_axis_max(double v) { min_axis_max_ = v; }
    // Fill the area under each trace with a translucent wash (nice for the
    // single/low-series memory & i/o graphs; left off for the 128 CPU lines).
    void set_fill(bool on) { fill_ = on; }

    void reset();

private:
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    double current_axis_max() const;

    int history_;
    int span_seconds_;
    bool percent_mode_ = true;
    bool fill_ = false;
    double fixed_max_ = 0.0;
    double min_axis_max_ = 1.0;
    std::function<std::string(double)> value_formatter_;

    std::vector<std::deque<double>> series_;
    std::vector<Gdk::RGBA> colors_;
};
