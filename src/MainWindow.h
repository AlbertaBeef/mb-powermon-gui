// Top-level window: a GNOME System Monitor-styled view of mb-powermon's
// telemetry — two sections, Power and Temperature, each a scrolling graph with
// one colored trace per metric and a legend showing live values.
#pragma once

#include <gdkmm/rgba.h>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/expander.h>
#include <gtkmm/label.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "GraphArea.h"
#include "GraphControls.h"
#include "Probes.h"

class MainWindow : public Gtk::ApplicationWindow {
public:
    MainWindow();

private:
    bool on_tick();
    void on_about();  // shows the About dialog (branded with the embedded logo)

    // A per-device aggregate label spanning metrics [start, start+count) in the
    // aligned value vector, so the tick can recompute the device's summary
    // reading (mean for temperature, max for power).
    struct AggEntry {
        Gtk::Label* label;
        int start;
        int count;
    };

    // The legend widgets belonging to one device: its name label, its
    // aggregate label and its metric cells (including any that wrapped onto a
    // continuation line). Captured during construction because the legend is a
    // Gtk::Grid laid out by device, with nothing to walk back to afterwards.
    struct LegendRow {
        std::string device;
        // The device name label and its aggregate label. Shown whenever any of
        // the row's cells is, hidden with the last of them — a name and a
        // "max —" floating with no entries beside them reads as a fault.
        std::vector<Gtk::Widget*> head;
        // One per metric cell, carrying the index of the metric it draws, so a
        // cell can be hidden on its own. Needed because the Telemetry switches
        // hide PART of a row: turning INA228 off takes the shunt cell off a
        // card that keeps its own sensors. The whole-row case (the device filter)
        // still works — every cell simply resolves to hidden.
        struct Cell {
            Gtk::Widget* widget;
            int metric;
        };
        std::vector<Cell> cells;
    };

    // Every graph in the window, in the order the sections appear. The Graphs
    // controls drive all of them at once and must not miss one.
    std::vector<GraphArea*> all_graphs();
    void apply_range_mode();
    // Auto, or a fixed window in minutes. The sample buffers are sized for the
    // top of that range, so this only changes what is drawn.
    void apply_time_range();
    // Hide the unticked devices' series AND their legend rows, on every graph.
    // Retroactive, and it re-scales the axes.
    void apply_graph_filter();

    // Builds a "graph + legend of live values" section. `metrics` fixes the
    // traces/rows; `value_fmt` renders each legend value. The GraphArea and the
    // per-row value labels are returned via out-params for the tick to update.
    // When `agg_out` is non-null, each device row also gets an aggregate label
    // (placed after the device name, before its metric entries) collected there;
    // the tick fills in its text.
    Gtk::Widget& build_metric_section(const std::vector<MetricInfo>& metrics,
                                      const std::vector<Gdk::RGBA>& colors,
                                      bool percent_temp_axis,
                                      std::function<std::string(double)> value_fmt,
                                      GraphArea*& graph_out,
                                      std::vector<Gtk::Label*>& value_labels_out,
                                      const char* empty_note,
                                      std::vector<AggEntry>* agg_out = nullptr,
                                      std::vector<LegendRow>* rows_out = nullptr,
                                      double min_axis_max = 10.0);
    // Per-metric colors: every metric of a device shares the device's color.
    std::vector<Gdk::RGBA> colors_for(const std::vector<MetricInfo>& metrics) const;

    std::vector<Gdk::RGBA> device_palette_;
    Gtk::Expander& make_section(const char* title, Gtk::Widget& content,
                                bool expanded = true);

    GraphControls* controls_ = nullptr;
    // The Paned's start child: the Box that WRAPS controls_ and carries its
    // 12 px margin. Hiding controls_ alone would leave that margin as an empty
    // strip, which is why this is a member where the sibling needs none — there
    // the panel is the start child itself.
    Gtk::Box* side_ = nullptr;
    // Header-bar toggle for the control pane, and the one place the pane is
    // hidden. Purely chrome: nothing is rebuilt or reset, so every control keeps
    // its state and the graphs keep their history across a hide/show.
    Gtk::ToggleButton* panel_toggle_ = nullptr;
    void set_panel_visible(bool on);
    // Every legend grid, for the Legends on/off toggle.
    std::vector<Gtk::Widget*> legend_grids_;

    Probes probes_;
    std::int64_t last_time_us_ = 0;
    Gtk::AboutDialog about_dialog_;
    bool about_ready_ = false;  // configured lazily on first open

    GraphArea* power_graph_ = nullptr;
    std::vector<Gtk::Label*> power_values_;
    std::vector<AggEntry> power_max_labels_;
    std::vector<LegendRow> power_rows_;

    GraphArea* temp_graph_ = nullptr;
    std::vector<Gtk::Label*> temp_values_;
    GraphArea* freq_graph_ = nullptr;
    std::vector<Gtk::Label*> freq_values_;
    std::vector<AggEntry> freq_agg_labels_;
    std::vector<LegendRow> freq_rows_;
    std::vector<AggEntry> temp_agg_labels_;
    std::vector<LegendRow> temp_rows_;

    // Accumulated energy from the INA228 hardware accumulators. Stays null on a
    // host with no shunts — the section is only built when the family is
    // non-empty, and this would otherwise be a zero-series graph.
    GraphArea* accum_graph_ = nullptr;
    std::vector<Gtk::Label*> accum_values_;
    std::vector<AggEntry> accum_sum_labels_;
    std::vector<LegendRow> accum_rows_;
    GraphArea* vbus_graph_ = nullptr;
    std::vector<Gtk::Label*> vbus_values_;
    std::vector<AggEntry> vbus_min_labels_;
    std::vector<LegendRow> vbus_rows_;
    GraphArea* curr_graph_ = nullptr;
    std::vector<Gtk::Label*> curr_values_;
    std::vector<AggEntry> curr_absmax_labels_;
    std::vector<LegendRow> curr_rows_;
    // Inline supply meter (POWER-Z): whole-board V / I / W, on their own graphs
    // rather than as extra series on the accelerator ones — a 19.9 V board
    // input or 22 W of board draw would flatten the card traces beside it.
    GraphArea* sysvbus_graph_ = nullptr;
    std::vector<Gtk::Label*> sysvbus_values_;
    std::vector<AggEntry> sysvbus_min_labels_;
    std::vector<LegendRow> sysvbus_rows_;
    GraphArea* syscurr_graph_ = nullptr;
    std::vector<Gtk::Label*> syscurr_values_;
    std::vector<AggEntry> syscurr_absmax_labels_;
    std::vector<LegendRow> syscurr_rows_;
    GraphArea* syspower_graph_ = nullptr;
    std::vector<Gtk::Label*> syspower_values_;
    std::vector<AggEntry> syspower_max_labels_;
    std::vector<LegendRow> syspower_rows_;
};
