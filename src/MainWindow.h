// Top-level window: a GNOME System Monitor-styled view of mb-powermon's
// telemetry — two sections, Power and Temperature, each a scrolling graph with
// one colored trace per metric and a legend showing live values.
#pragma once

#include <gdkmm/rgba.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/expander.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "GraphArea.h"
#include "Probes.h"

class MainWindow : public Gtk::ApplicationWindow {
public:
    MainWindow();

private:
    bool on_tick();

    // A per-device aggregate label spanning metrics [start, start+count) in the
    // aligned value vector, so the tick can recompute the device's summary
    // reading (mean for temperature, max for power).
    struct AggEntry {
        Gtk::Label* label;
        int start;
        int count;
    };

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
                                      std::vector<AggEntry>* agg_out = nullptr);
    // Per-metric colors: every metric of a device shares the device's color.
    std::vector<Gdk::RGBA> colors_for(const std::vector<MetricInfo>& metrics) const;

    std::vector<Gdk::RGBA> device_palette_;
    Gtk::Expander& make_section(const char* title, Gtk::Widget& content);

    Probes probes_;
    std::int64_t last_time_us_ = 0;

    GraphArea* power_graph_ = nullptr;
    std::vector<Gtk::Label*> power_values_;
    std::vector<AggEntry> power_max_labels_;

    GraphArea* temp_graph_ = nullptr;
    std::vector<Gtk::Label*> temp_values_;
    std::vector<AggEntry> temp_avg_labels_;
};
