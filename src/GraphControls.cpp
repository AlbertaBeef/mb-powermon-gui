#include "GraphControls.h"

#include <gtkmm/adjustment.h>

namespace {
// Past ~5 a row runs wider than the window. Same number the graph legends
// wrap at, for the same reason.
constexpr int kMaxPerRow = 5;
// Width of every row's leading label, so the rows line up in a column.
constexpr int kLabelChars = 12;

Gtk::Box* labelled_row(const char* text, Gtk::Align valign = Gtk::Align::CENTER) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    auto* lbl = Gtk::make_managed<Gtk::Label>(text);
    lbl->set_xalign(0.0);
    lbl->set_width_chars(kLabelChars);
    lbl->set_valign(valign);
    row->append(*lbl);
    return row;
}
}  // namespace

GraphControls::GraphControls() : Gtk::Box(Gtk::Orientation::VERTICAL, 8) {
    // ---- Accelerators ----
    // One row per card: its name, then an Enabled switch. The sibling's rows
    // carry that card's own controls after the switch; there is nothing to
    // configure here, so the row is just the two.
    //
    // Independent checkboxes, not a radio group: the point is comparing a
    // chosen few on one plot, so "Hailo and Axelera, not the other two" has to
    // be expressible. Cards only — the meters have their own switches in
    // Telemetry, and having them in both places would be two controls for one
    // thing.
    {
        accel_frame_ = Gtk::make_managed<Gtk::Frame>("Accelerators");
        auto* abox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        abox->set_margin(8);
        accel_grid_ = Gtk::make_managed<Gtk::Grid>();
        accel_grid_->set_row_spacing(6);
        accel_grid_->set_column_spacing(12);
        abox->append(*accel_grid_);
        accel_frame_->set_child(*abox);
        // Hidden until MainWindow says which cards it found; a host with none
        // gets no section rather than an empty frame.
        accel_frame_->set_visible(false);
        append(*accel_frame_);
    }

    auto* graphs = Gtk::make_managed<Gtk::Frame>("Graphs");
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    box->set_margin(8);

    // --- legends on/off ---
    // Display only: the traces, the axis and the readings are untouched. Worth
    // having because a device with many metrics makes the legend taller than
    // the plot it describes.
    {
        auto* row = labelled_row("Legends");
        show_legends_.set_active(true);
        show_legends_.set_tooltip_text(
            "Show the per-device legend under each graph. Hiding it frees the "
            "vertical space for the plots; the traces and the axis are "
            "unaffected.");
        row->append(show_legends_);
        conns_.push_back(show_legends_.signal_toggled().connect(
            [this] { sig_legends_.emit(); }));
        box->append(*row);
    }

    // --- how each axis responds to its data ---
    {
        auto* row = labelled_row("Values Range");
        range_max_.set_group(range_fixed_);
        range_dynamic_.set_group(range_fixed_);
        range_max_.set_active(true);   // current behaviour is the default

        range_fixed_.set_tooltip_text(
            "Leave every axis at its resting top — 100 °C for temperature, the "
            "per-graph floor elsewhere. Readings above it are clipped and draw "
            "flat along the top edge. This is what the graphs did originally.");
        range_max_.set_tooltip_text(
            "Start at the resting top and grow to 10 % above the highest "
            "reading once the data reaches it. The axis never shrinks back, so "
            "successive sessions stay comparable on one scale.");
        range_dynamic_.set_tooltip_text(
            "Scale to the data in both directions — always 10 % above the "
            "highest reading in the window, however small. Fills the plot, but "
            "the scale moves as the data does, so two sessions are not "
            "directly comparable by eye.");

        row->append(range_fixed_);
        row->append(range_max_);
        row->append(range_dynamic_);
        box->append(*row);

        for (auto* b : {&range_fixed_, &range_max_, &range_dynamic_}) {
            conns_.push_back(b->signal_toggled().connect([this, b] {
                if (b->get_active()) sig_range_mode_.emit();
            }));
        }
    }

    // --- how much wall-clock time is on screen ---
    {
        auto* row = labelled_row("Time Range");
        time_auto_.set_active(false);
        time_auto_.set_tooltip_text(
            "Show exactly the time that has been collected: the traces fill the "
            "plot from the first sample and the axis widens as the session "
            "runs, up to the 30 minutes the graphs keep.");
        // 1..30 min. The buffer is sized for the top of this range, so moving
        // the window never throws samples away.
        time_minutes_.set_adjustment(Gtk::Adjustment::create(5, 1, 30, 1, 5));
        time_minutes_.set_numeric(true);
        time_minutes_.set_width_chars(3);
        time_minutes_.set_sensitive(true);   // Auto is off, so this is live
        time_minutes_.set_tooltip_text(
            "A fixed window, in minutes. The newest sample stays at the right "
            "edge, so successive sessions line up on the same time scale.");
        auto* unit = Gtk::make_managed<Gtk::Label>("min");
        unit->set_xalign(0.0);

        row->append(time_auto_);
        row->append(time_minutes_);
        row->append(*unit);
        box->append(*row);

        conns_.push_back(time_auto_.signal_toggled().connect([this] {
            time_minutes_.set_sensitive(!time_auto_.get_active());
            sig_time_range_.emit();
        }));
        conns_.push_back(time_minutes_.signal_value_changed().connect([this] {
            if (!time_auto_.get_active()) sig_time_range_.emit();
        }));
    }

    graphs->set_child(*box);
    append(*graphs);

    // ---- Telemetry ----
    // Which *instruments* to draw, as opposed to Graphs, which is about which
    // devices and how the axes behave. One row per meter, in discover() order —
    // the board-level meters first, then the per-rail shunts — which is also
    // the order their graphs appear in. A row is built only where the
    // instrument was found, so no control sits dead on a host without it.
    {
        auto* frame = Gtk::make_managed<Gtk::Frame>("Telemetry");
        auto* tbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        tbox->set_margin(8);

        auto instrument_row = [&](const char* title, Gtk::CheckButton& sw,
                                  const char* tip) {
            auto* row = labelled_row(title);
            sw.set_active(true);
            sw.set_tooltip_text(tip);
            row->append(sw);
            row->set_visible(false);   // until MainWindow finds the instrument
            conns_.push_back(sw.signal_toggled().connect(
                [this] { sig_graph_filter_.emit(); }));
            tbox->append(*row);
            return row;
        };

        powerz_row_ = instrument_row(
            "POWER-Z", powerz_enabled_,
            "Draw the POWER-Z KM003C on the System graphs. Unticking hides its "
            "traces, including the history already on screen, and drops them "
            "from the axis. The meter is read either way.");

    // --- PMD2 per-measurement filter ---
    {
        pmd2_row_ = labelled_row("PMD2", Gtk::Align::START);
        pmd2_grid_ = Gtk::make_managed<Gtk::Grid>();
        pmd2_grid_->set_column_spacing(12);
        pmd2_grid_->set_row_spacing(2);
        pmd2_row_->append(*pmd2_grid_);
        pmd2_row_->set_visible(false);
        tbox->append(*pmd2_row_);
    }


        ina228_row_ = instrument_row(
            "INA228", ina228_enabled_,
            "Draw the INA228 shunts on the Accelerator graphs. Unticking hides "
            "every shunt trace — power, voltage, current, energy and the "
            "monitor's own die temperature — including the history already on "
            "screen, and drops them from the axis. They are read either way.");

        frame->set_child(*tbox);
        append(*frame);
    }
}

GraphControls::~GraphControls() {
    for (auto& c : conns_) c.disconnect();
}

GraphArea::RangeMode GraphControls::range_mode() const {
    if (range_fixed_.get_active()) return GraphArea::RangeMode::Fixed;
    if (range_dynamic_.get_active()) return GraphArea::RangeMode::Dynamic;
    return GraphArea::RangeMode::Max;
}

void GraphControls::set_accelerators(const std::vector<std::string>& names) {
    if (!accel_grid_ || names.empty()) return;
    int row = 0;
    for (const auto& n : names) {
        // Name and switch in their own columns, so they line up down the
        // section — the same two columns the sibling's card rows start with.
        auto* lbl = Gtk::make_managed<Gtk::Label>(n);
        lbl->set_xalign(0.0);
        lbl->set_width_chars(kLabelChars);
        accel_grid_->attach(*lbl, 0, row, 1, 1);

        auto* b = Gtk::make_managed<Gtk::CheckButton>("Enabled");
        b->set_active(true);   // everything shown by default
        b->set_tooltip_text(
            "Draw " + n + " on the graphs. Unticking hides its traces — "
            "including the history already on screen — and drops them from the "
            "axis calculation, so the remaining cards fill the plot. It is "
            "still measured either way; its legend values keep updating.");
        accel_grid_->attach(*b, 1, row, 1, 1);
        accel_boxes_[n] = b;
        conns_.push_back(
            b->signal_toggled().connect([this] { sig_graph_filter_.emit(); }));
        ++row;
    }
    accel_frame_->set_visible(true);
}

// Unknown device ⇒ visible. A metric whose device has no checkbox has no bit
// to consult, and hiding it would be the control lying about its own state.
bool GraphControls::device_shown(const std::string& device) const {
    const auto it = accel_boxes_.find(device);
    return it == accel_boxes_.end() ? true : it->second->get_active();
}

void GraphControls::set_pmd2_measurements(const std::vector<std::string>& total,
                                          const std::vector<std::string>& groups,
                                          const std::vector<std::string>& rails) {
    if (!pmd2_grid_) return;
    if (total.empty() && groups.empty() && rails.empty()) return;

    auto add = [&](const std::string& n, int col, int row) {
        auto* b = Gtk::make_managed<Gtk::CheckButton>(n);
        b->set_active(true);
        b->set_tooltip_text(
            "Draw the PMD2's " + n + " on the System graphs — its watts, volts "
            "and amps together. Unticking hides those traces, including the "
            "history already on screen, and drops them from the axis. It is "
            "always measured either way.");
        pmd2_grid_->attach(*b, col, row, 1, 1);
        pmd2_boxes_[n] = b;
        conns_.push_back(
            b->signal_toggled().connect([this] { sig_graph_filter_.emit(); }));
    };

    // Row 0: the master switch, alone — it governs everything below it, and a
    // reading on the same line would look like one more peer of the rails.
    int row = 0, col = 0;
    pmd2_enable_.set_active(true);
    pmd2_enable_.set_tooltip_text(
        "Draw the PMD2 at all. Unticking takes every one of its traces off the "
        "System graphs in one go and greys the boxes below — which keep their "
        "state, so a chosen subset comes back when you tick this again. The "
        "meter is read regardless.");
    pmd2_grid_->attach(pmd2_enable_, col, row, 1, 1);
    conns_.push_back(pmd2_enable_.signal_toggled().connect([this] {
        const bool on = pmd2_enable_.get_active();
        for (auto& kv : pmd2_boxes_) kv.second->set_sensitive(on);
        sig_graph_filter_.emit();
    }));
    // Row 1: the board total and the group subtotals — the summary tier, read
    // as TOTAL and the three groups it splits into.
    if (!total.empty() || !groups.empty()) {
        ++row;
        col = 0;
        for (const auto& n : total) add(n, col++, row);
        for (const auto& n : groups) add(n, col++, row);
    }

    // Rows 2+: the individual rails, wrapped.
    if (!rails.empty()) {
        ++row;
        col = 0;
        for (const auto& n : rails) {
            if (col == kMaxPerRow) { col = 0; ++row; }
            add(n, col++, row);
        }
    }
    pmd2_row_->set_visible(true);
}

void GraphControls::set_ina228_present(bool present) {
    if (ina228_row_) ina228_row_->set_visible(present);
}
void GraphControls::set_powerz_present(bool present) {
    if (powerz_row_) powerz_row_->set_visible(present);
}

bool GraphControls::pmd2_shown(const std::string& measurement) const {
    // Only ever asked about a PMD2 metric, so Enable can answer for all of
    // them. An unknown name stays visible — a measurement with no checkbox has
    // no bit to consult.
    if (!pmd2_enable_.get_active()) return false;
    const auto it = pmd2_boxes_.find(measurement);
    return it == pmd2_boxes_.end() ? true : it->second->get_active();
}
