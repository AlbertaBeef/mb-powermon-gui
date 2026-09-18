// The "Graphs" and "Telemetry" control frames, ported from mb-benchmark-gui's
// ControlPanel.
//
// That app has a whole left-hand panel — models, cards, Start/Stop — and this
// frame is one section of it. Here there is nothing to configure but the
// graphs, so it is the frame alone, sitting above the sections rather than in
// a Paned: a monitor window wants its width for plots.
//
// Everything here is display-only. Nothing it does reaches Probes: every
// reading is still taken, and a hidden trace is hidden, never dropped.
#pragma once

#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/frame.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>

#include <map>
#include <string>
#include <vector>

#include "GraphArea.h"

// A Box of frames rather than one Frame: "Graphs" is about which cards and how
// the axes behave, "Telemetry" about which instruments are drawn at all. Same
// split, and the same two headings, as the sibling's panel.
class GraphControls : public Gtk::Box {
public:
    GraphControls();
    ~GraphControls() override;

    // How every graph's axis top should respond to its data.
    GraphArea::RangeMode range_mode() const;
    sigc::signal<void()>& signal_range_mode_changed() { return sig_range_mode_; }

    // How much wall-clock time is on screen. Auto means "whatever has been
    // collected", which is what makes a young session visible at all: a fixed
    // window anchors the newest sample at the right edge, so a minute of data
    // in a ten-minute window is a stub in the last tenth of the plot.
    bool time_range_auto() const { return time_auto_.get_active(); }
    int time_range_minutes() const { return time_minutes_.get_value_as_int(); }
    sigc::signal<void()>& signal_time_range_changed() { return sig_time_range_; }

    // Which accelerators' traces to draw — any subset, all ticked by default.
    // mb-benchmark-gui hardcodes its five cards from the Accel enum; there is no
    // such enum here, so MainWindow passes the discovered device names that
    // util::device_accent() recognises as cards. **Instruments are deliberately
    // not in this list** — the PMD2, the POWER-Z and the INA228 shunts are
    // governed by their own Enabled switch in Telemetry, and a checkbox here as
    // well would be a second control for the same thing that could contradict
    // it.
    void set_accelerators(const std::vector<std::string>& names);
    // Takes a metric's device name. An instrument, or anything else with no
    // checkbox, has no bit to consult and stays visible — its own Telemetry
    // switch decides instead.
    bool device_shown(const std::string& device) const;
    sigc::signal<void()>& signal_graph_filter_changed() { return sig_graph_filter_; }

    // Legends ("palettes") shown under each graph. Purely a display toggle:
    // values keep updating and nothing is dropped from the axis.
    bool legends_shown() const { return show_legends_.get_active(); }
    sigc::signal<void()>& signal_legends_changed() { return sig_legends_; }

    // PMD2 per-measurement filter, one checkbox per measurement POINT rather
    // than per series: ticking ATX12V governs its watts, volts and amps
    // together, since they are three views of one rail. Stays hidden until
    // something publishes PMD2 metrics.
    //
    // Three tiers, one per line so the row reads as the hierarchy it is:
    // `Enable` and the board total, then the group subtotals, then the
    // individual rails wrapped at kMaxPerRow. MainWindow derives the split from
    // the metrics; see its comment for how.
    void set_pmd2_measurements(const std::vector<std::string>& total,
                               const std::vector<std::string>& groups,
                               const std::vector<std::string>& rails);
    bool pmd2_shown(const std::string& measurement) const;

    // The other two instruments in the Telemetry section. Each is a master
    // switch only — they publish a handful of series apiece, where the PMD2
    // publishes 34 and earns per-measurement boxes. A row appears only where
    // the instrument was actually found, so no control sits dead.
    void set_ina228_present(bool present);
    void set_powerz_present(bool present);
    bool ina228_shown() const { return ina228_enabled_.get_active(); }
    bool powerz_shown() const { return powerz_enabled_.get_active(); }

private:
    // Max is the default: it is the behaviour the graphs have always had, and
    // the one that never clips.
    Gtk::CheckButton range_fixed_{"Fixed"};
    Gtk::CheckButton range_max_{"Max"};
    Gtk::CheckButton range_dynamic_{"Dynamic"};

    // Auto off, 5 minutes: a fixed window is what makes two sessions
    // comparable. The spin is insensitive while Auto is ticked rather than
    // hidden, so the value it would take is visible before switching to it.
    Gtk::CheckButton time_auto_{"Auto"};
    Gtk::SpinButton time_minutes_;

    Gtk::Frame* accel_frame_ = nullptr;   // the whole section, hidden when no cards
    Gtk::Grid* accel_grid_ = nullptr;
    std::map<std::string, Gtk::CheckButton*> accel_boxes_;

    Gtk::CheckButton show_legends_{"Enabled"};

    Gtk::Box* pmd2_row_ = nullptr;   // the whole labelled row, hidden when absent
    Gtk::Grid* pmd2_grid_ = nullptr;
    // Master switch for the whole meter. Unticking greys the per-measurement
    // boxes rather than clearing them, so a chosen subset survives being
    // switched off and back on.
    Gtk::CheckButton pmd2_enable_{"Enabled"};

    // INA228 shunts and the POWER-Z meter: one switch each, rows hidden until
    // MainWindow says the instrument is present.
    Gtk::Box* ina228_row_ = nullptr;
    Gtk::CheckButton ina228_enabled_{"Enabled"};
    Gtk::Box* powerz_row_ = nullptr;
    Gtk::CheckButton powerz_enabled_{"Enabled"};
    std::map<std::string, Gtk::CheckButton*> pmd2_boxes_;

    sigc::signal<void()> sig_range_mode_;
    sigc::signal<void()> sig_time_range_;
    sigc::signal<void()> sig_graph_filter_;
    sigc::signal<void()> sig_legends_;

    // Every handler that reads another widget must live here: widget members
    // are destroyed in reverse declaration order, and a handler firing during
    // teardown can otherwise touch one that is already gone. The destructor
    // body is the last moment they are all alive.
    std::vector<sigc::connection> conns_;
};
