#include "MainWindow.h"

#include <glib.h>
#include <glibmm/main.h>
#include <gdkmm/display.h>
#include <gdkmm/texture.h>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <gtkmm/shortcut.h>
#include <gtkmm/shortcutaction.h>   // Gtk::CallbackAction lives here
#include <gtkmm/shortcutcontroller.h>
#include <gtkmm/shortcuttrigger.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/togglebutton.h>

#include <gdk/gdkkeysyms.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "util.h"

namespace {
// Legend cells drop the family suffix: on the Power graph every cell is watts,
// so "ATX12V POWER" says it twice and costs ~45 px of row width doing it. Each
// family has its own graph, so the suffix is redundant *by construction* and
// cannot make two cells ambiguous. DISPLAY ONLY — mb-benchmark-gui's CSV keeps
// the full label, because its columns share one namespace across families.
std::string legend_short(std::string lbl) {
    for (const char* fam : {" POWER", " VBUS", " CURRENT", " ENERGY",
                            " CHARGE", " TEMP"}) {
        const size_t n = std::strlen(fam);
        if (lbl.size() > n && lbl.compare(lbl.size() - n, n, fam) == 0)
            return lbl.substr(0, lbl.size() - n);
    }
    return lbl;
}
// Past this a device's legend row runs wider than the window, and the graph
// inherits that width: GraphArea anchors its trace newest-at-the-right-edge,
// so an over-wide plot puts a young history off screen entirely.
constexpr int kMaxLegendCellsPerRow = 5;
constexpr int kIntervalMs = 1000;
// The graphs *keep* 30 minutes — the top of the Time Range control — and draw
// whichever window that control asks for, 5 minutes by default. Buffering the
// maximum is what lets the window be widened again without a gap: narrowing it
// then discards nothing.
constexpr int kSpanSeconds = 300;       // default window drawn, 5 min
constexpr int kMaxSpanSeconds = 1800;   // Time Range maximum, 30 min
constexpr int kHistory = kMaxSpanSeconds + 1;
constexpr double kTempAxisMax = 100.0;  // °C, matches mb-powermon's default

std::string fmt_temp(double v) {
    if (std::isnan(v)) return "—";
    char b[24];
    std::snprintf(b, sizeof(b), "%.0f°C", v);
    return b;
}
std::string fmt_freq(double v) {
    if (std::isnan(v)) return "—";
    char b[24];
    std::snprintf(b, sizeof(b), "%.0f MHz", v);
    return b;
}
std::string fmt_power(double v) {
    if (std::isnan(v)) return "—";
    char b[24];
    std::snprintf(b, sizeof(b), "%.2f W", v);
    return b;
}
// Joules. Accumulates without bound over a session, so no fixed width.
// Volts. Three decimals on purpose: the question this graph answers is a
// ~200 mV sag on a 3.3 V rail, and %.1f would quantise that away entirely.
// Amps, signed. Three decimals: the interesting range is 0.5-4 A and the
// question is a few hundred mA of delta.
std::string fmt_amps(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%+.3f", v);
    return b;
}
std::string fmt_volts(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}
std::string fmt_joules(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}
std::string fmt_temp_axis(double v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%.0f°C", v);
    return b;
}
}  // namespace

MainWindow::MainWindow() {
    set_title("NPU Power and Temperature Monitoring GUI");
    // Wider than it was (940x620): the controls pane takes ~620 px of it, and
    // the graphs must keep about the width they had rather than pay for it.
    set_default_size(1560, 820);

    // Teal title bar (brand Primary), white title text.
    auto* header = Gtk::make_managed<Gtk::HeaderBar>();
    auto* title = Gtk::make_managed<Gtk::Label>("NPU Power and Temperature Monitoring GUI");
    title->add_css_class("title");
    header->set_title_widget(*title);

    // Collapse the control pane and give its width to the graphs. The panel is
    // set-and-forget, so it is ~40 % of the window spent on controls nobody is
    // touching once the graphs are configured.
    //
    // ONE icon, on a ToggleButton, rather than swapping show/hide icons:
    // `sidebar-show-symbolic` is in both Yaru (the active theme) and Adwaita
    // (the fallback), while `sidebar-hide-symbolic` is **Yaru-only** and would
    // render blank for anyone on stock Adwaita. A ToggleButton draws its own
    // checked state, so there is nothing to swap.
    panel_toggle_ = Gtk::make_managed<Gtk::ToggleButton>();
    panel_toggle_->set_icon_name("sidebar-show-symbolic");
    panel_toggle_->set_tooltip_text("Show the control panel (Ctrl+B)");
    panel_toggle_->add_css_class("flat");
    panel_toggle_->set_active(true);   // the panel starts visible
    panel_toggle_->signal_toggled().connect(
        [this] { set_panel_visible(panel_toggle_->get_active()); });
    header->pack_start(*panel_toggle_);

    // About button (right side of the header) — opens the branded About dialog.
    auto* about_btn = Gtk::make_managed<Gtk::Button>();
    about_btn->set_icon_name("help-about-symbolic");
    about_btn->set_tooltip_text("About");
    about_btn->add_css_class("flat");
    about_btn->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_about));
    header->pack_end(*about_btn);
    set_titlebar(*header);

    auto css = Gtk::CssProvider::create();
    css->load_from_data(
        "headerbar { background: #64A19D; box-shadow: none; }"
        "headerbar label.title { color: #FFFFFF; font-weight: bold; }"
        // About dialog: its name label is a selectable label that auto-selects
        // on open; hide that selection highlight so the name reads plainly.
        ".mb-about selection { background-color: transparent; color: inherit; }");
    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(), css,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    std::vector<std::string> notes;
    probes_.discover(&notes);
    for (const auto& n : notes) g_message("probe: %s", n.c_str());

    device_palette_ = util::make_palette(std::max(1, probes_.device_count()));

    // Let a device with a color_alias (a mapped INA228, e.g. "INA228 - Hailo")
    // reuse the swatch of the device it names ("Hailo"), so the external shunt
    // series matches its accelerator across both graphs.
    {
        const int nd = probes_.device_count();
        std::vector<std::string> dname(nd), dalias(nd);
        auto index_devs = [&](const std::vector<MetricInfo>& ms) {
            for (const auto& m : ms)
                if (m.device >= 0 && m.device < nd) {
                    dname[m.device] = m.device_name;
                    if (!m.color_alias.empty()) dalias[m.device] = m.color_alias;
                }
        };
        index_devs(probes_.temp_metrics());
        index_devs(probes_.power_metrics());

        // Fixed colour per accelerator, before the alias pass — a mapped
        // INA228 then inherits its card's colour rather than a palette slot.
        // Shared with mb-benchmark-gui via util.h so a card looks the same in
        // both apps.
        for (int i = 0; i < nd; ++i) {
            Gdk::RGBA c;
            if (util::device_accent(dname[i], c)) device_palette_[i] = c;
        }
        for (int i = 0; i < nd; ++i) {
            if (dalias[i].empty()) continue;
            for (int j = 0; j < nd; ++j)
                if (j != i && dname[j] == dalias[i]) {
                    device_palette_[i] = device_palette_[j];
                    break;
                }
        }
    }
    last_time_us_ = g_get_monotonic_time();

    // Controls left, graphs right — the same Gtk::Paned arrangement as
    // mb-benchmark-gui, so the two apps are laid out alike. The controls are
    // the only thing in that pane here; mb-benchmark-gui's carries the model
    // lists and Start/Stop above the same Graphs frame.
    auto* paned = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
    paned->set_position(620);
    set_child(*paned);

    side_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    side_->set_margin(12);
    controls_ = Gtk::make_managed<GraphControls>();
    // Natural height at the top of the pane: a frame stretched down the window
    // would be mostly empty box with its rows stranded at the top.
    controls_->set_valign(Gtk::Align::START);
    side_->append(*controls_);
    paned->set_start_child(*side_);
    paned->set_resize_start_child(false);
    // Never allocate the controls less than their minimum: GTK4's default
    // shrink-start-child lets the handle squeeze a child below its size
    // request, and a widget rendered under its minimum anchors its contents
    // unpredictably instead of staying flush left.
    paned->set_shrink_start_child(false);

    // Ctrl+B, the keyboard equivalent of the header toggle. This is the FIRST
    // and only keyboard shortcut in either app, and it is a ShortcutController
    // on the window rather than an action + set_accels_for_action() because
    // nothing here holds the Gtk::Application — main() uses
    // make_window_and_run(), which keeps it to itself.
    //
    // It flips the BUTTON rather than calling set_panel_visible() directly, so
    // there is one code path and the button can never disagree with the pane.
    {
        auto sc = Gtk::ShortcutController::create();
        // MANAGED, not LOCAL: the shortcut must fire wherever focus happens to
        // sit — a spin button in the panel, a graph, the window background.
        sc->set_scope(Gtk::ShortcutScope::MANAGED);
        sc->add_shortcut(Gtk::Shortcut::create(
            Gtk::KeyvalTrigger::create(GDK_KEY_b, Gdk::ModifierType::CONTROL_MASK),
            Gtk::CallbackAction::create(
                [this](Gtk::Widget&, const Glib::VariantBase&) {
                    panel_toggle_->set_active(!panel_toggle_->get_active());
                    return true;
                })));
        add_controller(sc);
    }

    // Fill layout (not a natural-height scroller): the sections share the
    // window's vertical space so the graphs grow with the window. Deliberately
    // NOT mb-benchmark-gui's ScrolledWindow — that app has twelve sections and
    // needs one; nine fill this window without scrolling.
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    root->set_margin(12);
    paned->set_end_child(*root);
    paned->set_resize_end_child(true);

    // Voltage and Current lead deliberately: they are the independent
    // measurements — the INA228 measures VBUS and the shunt drop and derives
    // POWER as their product — so a sagging rail reads top-to-bottom, current
    // rising and voltage falling before power is the result. Both collapsed by
    // default: on a healthy supply they are flat lines, and only interesting
    // when they are not.
    // The System triplet leads, and reads voltage -> current -> power like the
    // Accelerator one below it: the meter measures V and I and derives W, so a
    // sagging supply reads top-to-bottom. Board total first, then the per-rail
    // breakdown inside it. Deliberately separate graphs, not extra series:
    // 19.9 V of board input beside a 3.3 V card rail, or 22 W of board draw
    // beside a card's 0.85 W, flattens the trace that matters.
    if (!probes_.sysvoltage_metrics().empty()) {
        root->append(make_section(
            "System Voltage (V)",
            build_metric_section(probes_.sysvoltage_metrics(),
                                 colors_for(probes_.sysvoltage_metrics()),
                                 /*percent_temp_axis=*/false, fmt_volts,
                                 sysvbus_graph_, sysvbus_values_,
                                 "No inline supply meter found.",
                                 &sysvbus_min_labels_, &sysvbus_rows_,
                                 /*min_axis_max=*/20.0),
            /*expanded=*/false));
    }

    if (!probes_.syscurrent_metrics().empty()) {
        root->append(make_section(
            "System Current (A)",
            build_metric_section(probes_.syscurrent_metrics(),
                                 colors_for(probes_.syscurrent_metrics()),
                                 /*percent_temp_axis=*/false, fmt_amps,
                                 syscurr_graph_, syscurr_values_,
                                 "No inline supply meter found.",
                                 &syscurr_absmax_labels_, &syscurr_rows_,
                                 /*min_axis_max=*/2.0),
            /*expanded=*/false));
    }

    if (!probes_.syspower_metrics().empty()) {
        root->append(make_section(
            "System Power (W)",
            build_metric_section(probes_.syspower_metrics(),
                                 colors_for(probes_.syspower_metrics()),
                                 /*percent_temp_axis=*/false, fmt_power,
                                 syspower_graph_, syspower_values_,
                                 "No inline supply meter found.",
                                 &syspower_max_labels_, &syspower_rows_,
                                 /*min_axis_max=*/10.0),
            /*expanded=*/true));
    }

    if (!probes_.voltage_metrics().empty()) {
        root->append(make_section(
            "Accelerator Voltage (V)",
            build_metric_section(probes_.voltage_metrics(),
                                 colors_for(probes_.voltage_metrics()),
                                 /*percent_temp_axis=*/false, fmt_volts,
                                 vbus_graph_, vbus_values_,
                                 "No INA228 shunts, so no rail voltage.",
                                 &vbus_min_labels_, &vbus_rows_,
                                 // 3.4 floor keeps a nominal 3.3 V rail off the
                                 // top edge without flattening the sag.
                                 /*min_axis_max=*/3.4),
            /*expanded=*/false));
    }

    // Reads NEGATIVE on this rig — IN+/IN- are wired the other way round — and
    // is shown as measured rather than abs()'d, so a later rewire stays visible
    // rather than being silently absorbed.
    if (!probes_.current_metrics().empty()) {
        root->append(make_section(
            "Accelerator Current (A)",
            build_metric_section(probes_.current_metrics(),
                                 colors_for(probes_.current_metrics()),
                                 /*percent_temp_axis=*/false, fmt_amps,
                                 curr_graph_, curr_values_,
                                 "No INA228 shunts, so no rail current.",
                                 &curr_absmax_labels_, &curr_rows_,
                                 /*min_axis_max=*/1.0),
            /*expanded=*/false));
    }

    root->append(make_section(
        "Accelerator Power (W)",
        build_metric_section(probes_.power_metrics(),
                             colors_for(probes_.power_metrics()),
                             /*percent_temp_axis=*/false, fmt_power, power_graph_,
                             power_values_,
                             "No power source available — an INA228 shunt or a "
                             "vendor SDK session is needed for watts.",
                             &power_max_labels_, &power_rows_)));

    // Accumulated energy sits with Power because it is its integral.
    if (!probes_.energy_metrics().empty()) {
        root->append(make_section(
            "Accumulated Energy (J)",
            build_metric_section(probes_.energy_metrics(),
                                 colors_for(probes_.energy_metrics()),
                                 /*percent_temp_axis=*/false, fmt_joules,
                                 accum_graph_, accum_values_,
                                 "No INA228 shunts, so nothing accumulates.",
                                 &accum_sum_labels_, &accum_rows_,
                                 // Low floor: the accumulator starts at zero, so
                                 // a large axis would pin the trace to the
                                 // bottom edge and read as an empty graph.
                                 /*min_axis_max=*/10.0),
            /*expanded=*/false));
    }

    root->append(make_section(
        "Temperature (°C)",
        build_metric_section(probes_.temp_metrics(),
                             colors_for(probes_.temp_metrics()),
                             /*percent_temp_axis=*/true, fmt_temp, temp_graph_,
                             temp_values_, "No temperature sensors detected.",
                             &temp_agg_labels_, &temp_rows_)));

    // Clock, right after temperature because the two are read together: a
    // frequency that sags while a die heats is thermal throttling, and seeing
    // them adjacent is the whole point. Collapsed by default — it is
    // diagnostic rather than something to watch continuously.
    root->append(make_section(
        "Frequency (MHz)",
        build_metric_section(probes_.freq_metrics(),
                             colors_for(probes_.freq_metrics()),
                             /*percent_temp_axis=*/false, fmt_freq, freq_graph_,
                             freq_values_,
                             "No accelerator here reports a core clock. All "
                             "four M.2 cards can: Hailo via the extended device "
                             "information, DeepX per NPU via dxrt-cli, MemryX "
                             "per chip via the SDK, and Axelera per AI core via "
                             "axcmd --clock-all-actual.",
                             &freq_agg_labels_, &freq_rows_,
                             /*min_axis_max=*/1000.0),
        /*expanded=*/false));

    // Tell the controls which ACCELERATORS exist, so they can build one
    // checkbox each. Derived from the metrics rather than a hardcoded list, in
    // discovery order, duplicates dropped — a device appears in several
    // families.
    //
    // Cards only, gated on util::device_accent() — the same shared helper that
    // gives each card its colour, which recognises the five card names (and the
    // Qualcomm board's several spellings) while excluding the ambient probe.
    // **The instruments are deliberately left out**: the PMD2, the POWER-Z and
    // the INA228 shunts each have an Enabled switch in Telemetry, and a
    // checkbox here as well would be a second control for the same thing that
    // could contradict it. device_shown() returns true for anything with no
    // checkbox, so leaving them out hands them entirely to their own switch.
    //
    // The PMD2's own points go in three tiers — the board total, the group
    // subtotals, the individual rails — and those are DERIVED too, so a
    // firmware that renames or adds a rail needs no change here. Two
    // structural facts do the work: the TOTAL is the one power metric with no
    // family suffix, so legend_short() leaves it untouched where it trims
    // every other; and a RAIL is a point that also carries a voltage and a
    // current, because only the rails are measured — a group is a POWER-only
    // subtotal.
    {
        std::vector<std::string> devs;
        auto key = [](const MetricInfo& m) {
            std::string k = m.label;
            if (k.rfind(m.device_name + " ", 0) == 0)
                k = k.substr(m.device_name.size() + 1);
            return k;
        };
        auto collect_devs = [&](const std::vector<MetricInfo>& ms) {
            for (const auto& m : ms) {
                if (m.device_name.empty()) continue;
                Gdk::RGBA accent;
                if (!util::device_accent(m.device_name, accent)) continue;
                if (std::find(devs.begin(), devs.end(), m.device_name) ==
                    devs.end())
                    devs.push_back(m.device_name);
            }
        };
        collect_devs(probes_.sysvoltage_metrics());
        collect_devs(probes_.syscurrent_metrics());
        collect_devs(probes_.syspower_metrics());
        collect_devs(probes_.voltage_metrics());
        collect_devs(probes_.current_metrics());
        collect_devs(probes_.power_metrics());
        collect_devs(probes_.energy_metrics());
        collect_devs(probes_.temp_metrics());
        collect_devs(probes_.freq_metrics());
        controls_->set_accelerators(devs);

        std::vector<std::string> measured;   // points with a V reading = rails
        for (const auto& m : probes_.sysvoltage_metrics())
            if (m.device_name == "PMD2") measured.push_back(legend_short(key(m)));

        std::vector<std::string> total, groups, rails;
        for (const auto& m : probes_.syspower_metrics()) {
            if (m.device_name != "PMD2") continue;
            const std::string raw = key(m);
            const std::string k = legend_short(raw);
            std::vector<std::string>& bucket =
                (k == raw) ? total
                : (std::find(measured.begin(), measured.end(), k) != measured.end())
                      ? rails
                      : groups;
            if (std::find(bucket.begin(), bucket.end(), k) == bucket.end())
                bucket.push_back(k);
        }
        // Inert until a PMD2 probe is ported here: with no such metrics all
        // three are empty and the row stays hidden.
        controls_->set_pmd2_measurements(total, groups, rails);

        // The other two instruments get a switch each, and only if they are
        // here. INA228 is recognised by LABEL, not by device name: a mapped
        // shunt is folded onto its card, so its device_name is "Hailo" and only
        // the label still says INA228 (`<Card> INA228 POWER`). An unmapped one
        // keeps `INA228#<n>`, which the same test catches.
        bool ina228 = false, powerz = false;
        auto scan = [&](const std::vector<MetricInfo>& ms) {
            for (const auto& m : ms) {
                if (m.label.find("INA228") != std::string::npos) ina228 = true;
                if (m.device_name == "POWER-Z") powerz = true;
            }
        };
        scan(probes_.power_metrics());
        scan(probes_.voltage_metrics());
        scan(probes_.current_metrics());
        scan(probes_.energy_metrics());
        scan(probes_.temp_metrics());
        scan(probes_.syspower_metrics());
        scan(probes_.sysvoltage_metrics());
        scan(probes_.syscurrent_metrics());
        controls_->set_ina228_present(ina228);
        controls_->set_powerz_present(powerz);
    }

    controls_->signal_range_mode_changed().connect(
        sigc::mem_fun(*this, &MainWindow::apply_range_mode));
    controls_->signal_time_range_changed().connect(
        sigc::mem_fun(*this, &MainWindow::apply_time_range));
    controls_->signal_graph_filter_changed().connect(
        sigc::mem_fun(*this, &MainWindow::apply_graph_filter));
    controls_->signal_legends_changed().connect([this] {
        const bool vis = controls_->legends_shown();
        for (Gtk::Widget* g : legend_grids_)
            if (g) g->set_visible(vis);
    });

    // Seed every graph from the controls, so the widget defaults and the
    // control state cannot drift apart.
    apply_range_mode();
    apply_time_range();
    apply_graph_filter();

    Glib::signal_timeout().connect(sigc::mem_fun(*this, &MainWindow::on_tick),
                                   kIntervalMs);
}

Gtk::Expander& MainWindow::make_section(const char* title, Gtk::Widget& content,
                                        bool expanded) {
    auto* exp = Gtk::make_managed<Gtk::Expander>();
    auto* lbl = Gtk::make_managed<Gtk::Label>();
    lbl->set_markup(std::string("<b>") + title + "</b>");
    exp->set_label_widget(*lbl);
    exp->set_expanded(expanded);
    exp->set_margin_top(4);
    // Only claim vertical space while expanded; a collapsed expander with
    // vexpand=true would keep its share of the window as an empty gap. Bind
    // vexpand to the expanded state so collapsing gives the space back.
    exp->set_vexpand(exp->get_expanded());
    exp->property_expanded().signal_changed().connect(
        [exp]() { exp->set_vexpand(exp->get_expanded()); });
    auto* pad = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    pad->set_margin_top(6);
    pad->set_margin_start(4);
    pad->set_vexpand(true);
    pad->append(content);
    exp->set_child(*pad);
    return *exp;
}

std::vector<Gdk::RGBA> MainWindow::colors_for(
    const std::vector<MetricInfo>& metrics) const {
    std::vector<Gdk::RGBA> out;
    out.reserve(metrics.size());
    for (const auto& m : metrics) {
        int d = (m.device >= 0 &&
                 m.device < static_cast<int>(device_palette_.size()))
                    ? m.device
                    : 0;
        out.push_back(device_palette_.empty() ? util::rgb(0.5, 0.5, 0.5)
                                              : device_palette_[d]);
    }
    return out;
}

Gtk::Widget& MainWindow::build_metric_section(
    const std::vector<MetricInfo>& metrics, const std::vector<Gdk::RGBA>& colors,
    bool temp_axis, std::function<std::string(double)> value_fmt,
    GraphArea*& graph_out, std::vector<Gtk::Label*>& value_labels_out,
    const char* empty_note, std::vector<AggEntry>* agg_out,
    std::vector<LegendRow>* rows_out, double min_axis_max) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_vexpand(true);  // graph inside grows; legend keeps natural height

    const int n = static_cast<int>(metrics.size());

    auto* graph = Gtk::make_managed<GraphArea>(kHistory, kSpanSeconds);
    graph->set_percent_mode(false);
    if (temp_axis) {
        graph->set_fixed_max(kTempAxisMax);
        graph->set_value_formatter(fmt_temp_axis);
    } else {
        // Auto-scaling with a floor. The formatter passed in decides the unit,
        // so this same branch serves both the power and the energy graphs.
        // (It used to hardcode fmt_power, which was harmless while power was
        // the only caller and wrong the moment a second unit appeared.)
        graph->set_min_axis_max(min_axis_max);
        graph->set_value_formatter(value_fmt);
    }
    graph->set_series(std::vector<Gdk::RGBA>(colors.begin(), colors.begin() + n));
    box->append(*graph);
    graph_out = graph;

    if (n == 0) {
        auto* note = Gtk::make_managed<Gtk::Label>(empty_note);
        note->set_xalign(0.0);
        note->set_wrap(true);
        note->add_css_class("dim-label");
        note->set_margin_top(4);
        box->append(*note);
        return *box;
    }

    // One row per device: a bold device name, then that device's swatch+label+
    // value entries in aligned columns (Axelera 5, Hailo 2, ...).
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(3);
    grid->set_column_spacing(20);
    grid->set_halign(Gtk::Align::START);

    value_labels_out.assign(n, nullptr);
    if (rows_out) rows_out->clear();
    int i = 0, row = 0;
    while (i < n) {
        const int dev = metrics[i].device;
        const std::string& dname = metrics[i].device_name;
        const std::string& bdf = metrics[i].bdf;

        auto* dn = Gtk::make_managed<Gtk::Label>();
        std::string prefix = bdf.empty() ? "" : bdf + "  ";
        dn->set_markup(prefix + "<b>" + dname + "</b>");
        dn->set_xalign(0.0);
        dn->set_margin_end(6);
        grid->attach(*dn, 0, row, 1, 1);
        LegendRow lr;
        lr.device = dname;
        lr.head.push_back(dn);

        int col = 1;
        // Optional per-device aggregate, between the name and the metric entries.
        const int agg_start = i;
        Gtk::Label* agg_label = nullptr;
        if (agg_out) {
            agg_label = Gtk::make_managed<Gtk::Label>("—");
            agg_label->set_xalign(0.0);
            agg_label->set_margin_end(6);
            agg_label->add_css_class("dim-label");
            grid->attach(*agg_label, col, row, 1, 1);
            lr.head.push_back(agg_label);
            ++col;
        }

        // Wrap a device's cells rather than letting one row grow without
        // limit. A device with many metrics otherwise makes the grid far wider
        // than the window; the graph inherits that width and GraphArea anchors
        // its trace newest-at-the-right-edge, so early in a session the line
        // sits in the last few pixels of a plot that is mostly off screen and
        // the graph reads as empty.
        const int first_cell_col = col;
        int cells_in_row = 0;

        while (i < n && metrics[i].device == dev) {
            if (cells_in_row == kMaxLegendCellsPerRow) {
                ++row;                     // continuation line for this device
                col = first_cell_col;      // aligned under the first row's cells
                cells_in_row = 0;          // column 0 stays empty: one name per device
            }
            auto* cell =
                Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);

            Gdk::RGBA c = colors[i];
            auto* swatch = Gtk::make_managed<Gtk::DrawingArea>();
            swatch->set_content_width(16);
            swatch->set_content_height(12);
            swatch->set_valign(Gtk::Align::CENTER);
            swatch->set_draw_func(
                [c](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                    cr->rectangle(0.5, 0.5, w - 1, h - 1);
                    cr->set_source_rgb(c.get_red(), c.get_green(), c.get_blue());
                    cr->fill_preserve();
                    Gdk::RGBA ink = util::neutral::ink();
                    cr->set_source_rgba(ink.get_red(), ink.get_green(),
                                        ink.get_blue(), 0.3);
                    cr->set_line_width(1.0);
                    cr->stroke();
                });
            cell->append(*swatch);

            // Metric label with the device prefix stripped ("Hailo TS0" -> "TS0").
            std::string lbl = metrics[i].label;
            if (lbl.rfind(dname + " ", 0) == 0) lbl = lbl.substr(dname.size() + 1);
            auto* name = Gtk::make_managed<Gtk::Label>(legend_short(lbl));
            name->set_xalign(0.0);
            name->set_width_chars(4);
            cell->append(*name);

            auto* val = Gtk::make_managed<Gtk::Label>("—");
            val->set_xalign(1.0);
            val->set_width_chars(6);
            value_labels_out[i] = val;
            cell->append(*val);

            grid->attach(*cell, col, row, 1, 1);
            // Every wrapped cell joins the SAME LegendRow, and carries the
            // index of the metric it draws so it can be hidden on its own —
            // which is what a Telemetry switch needs, since that hides part of
            // a row rather than all of it.
            lr.cells.push_back({cell, i});
            ++col;
            ++cells_in_row;
            ++i;
        }
        if (agg_out) agg_out->push_back({agg_label, agg_start, i - agg_start});
        if (rows_out) rows_out->push_back(std::move(lr));
        ++row;
    }
    legend_grids_.push_back(grid);
    box->append(*grid);
    return *box;
}

// Hiding the Paned's start child is enough: GtkPaned gives the whole area to
// the remaining child and drops the handle. Detaching the child instead would
// re-parent live widgets and reset set_position(). It is side_ rather than
// controls_ so the wrapper's 12 px margin goes with it.
void MainWindow::set_panel_visible(bool on) {
    if (side_) side_->set_visible(on);
}

// In section order. A graph absent from this list silently keeps whatever the
// controls last left it at.
std::vector<GraphArea*> MainWindow::all_graphs() {
    std::vector<GraphArea*> v;
    for (GraphArea* g : {sysvbus_graph_, syscurr_graph_, syspower_graph_,
                         vbus_graph_, curr_graph_, power_graph_, accum_graph_,
                         temp_graph_, freq_graph_}) {
        if (g) v.push_back(g);
    }
    return v;
}

// Every graph shares one Values Range setting: mixing modes between graphs
// would make the plots answer different questions at the same moment.
void MainWindow::apply_range_mode() {
    const auto m = controls_->range_mode();
    for (GraphArea* g : all_graphs()) g->set_range_mode(m);
}

// Likewise one Time Range for all of them — two plots on different time scales
// cannot be read against each other, which is the whole point of stacking them.
void MainWindow::apply_time_range() {
    const bool automatic = controls_->time_range_auto();
    const int span = std::min(controls_->time_range_minutes() * 60, kMaxSpanSeconds);
    for (GraphArea* g : all_graphs()) {
        if (automatic) g->set_auto_time_span(true);
        else           g->set_time_span(span);
    }
}

// Hiding, never dropping: push() still receives every reading, so the filter is
// retroactive (the history already on screen goes too) and a hidden series is
// skipped by axis_range(), giving the remaining devices the whole plot.
void MainWindow::apply_graph_filter() {
    // A PMD2 metric carries its measurement point in the label
    // ("PMD2 ATX12V POWER"); strip the device prefix and the family suffix and
    // what is left is the key the checkboxes are built from.
    auto metric_shown = [&](const MetricInfo& mi) {
        if (!controls_->device_shown(mi.device_name)) return false;
        // Instrument switches, from the Telemetry section. INA228 is matched on
        // the label because a mapped shunt carries its CARD's device name; the
        // device filter above has already had its say, so this is an additional
        // gate rather than an alternative one.
        if (mi.label.find("INA228") != std::string::npos)
            return controls_->ina228_shown();
        if (mi.device_name == "POWER-Z") return controls_->powerz_shown();
        if (mi.device_name != "PMD2") return true;
        std::string k = mi.label;
        if (k.rfind(mi.device_name + " ", 0) == 0)
            k = k.substr(mi.device_name.size() + 1);
        return controls_->pmd2_shown(legend_short(k));
    };

    auto by_device = [&](GraphArea* g, const std::vector<MetricInfo>& m,
                         const std::vector<LegendRow>& rows) {
        if (g) {
            for (size_t i = 0; i < m.size(); ++i)
                g->set_series_visible(static_cast<int>(i), metric_shown(m[i]));
        }
        // The legend follows the traces cell by cell, not row by row. A
        // Telemetry switch hides PART of a row — turning INA228 off takes the
        // shunt cell off a card that keeps its own sensors — so hiding whole
        // rows would either leave a cell describing a trace that is gone, or
        // take away sensors that are still drawn. The head (device name +
        // aggregate) follows the last surviving cell: a name and a "max —"
        // with no entries beside them reads as a fault rather than a filter.
        for (const auto& r : rows) {
            bool any = false;
            for (const auto& c : r.cells) {
                const bool vis =
                    c.metric < static_cast<int>(m.size()) && metric_shown(m[c.metric]);
                if (c.widget) c.widget->set_visible(vis);
                any = any || vis;
            }
            for (Gtk::Widget* w : r.head)
                if (w) w->set_visible(any);
        }
    };
    by_device(sysvbus_graph_, probes_.sysvoltage_metrics(), sysvbus_rows_);
    by_device(syscurr_graph_, probes_.syscurrent_metrics(), syscurr_rows_);
    by_device(syspower_graph_, probes_.syspower_metrics(), syspower_rows_);
    by_device(vbus_graph_, probes_.voltage_metrics(), vbus_rows_);
    by_device(curr_graph_, probes_.current_metrics(), curr_rows_);
    by_device(power_graph_, probes_.power_metrics(), power_rows_);
    by_device(accum_graph_, probes_.energy_metrics(), accum_rows_);
    by_device(temp_graph_, probes_.temp_metrics(), temp_rows_);
    by_device(freq_graph_, probes_.freq_metrics(), freq_rows_);
}

bool MainWindow::on_tick() {
    const std::int64_t now = g_get_monotonic_time();
    (void)now;
    last_time_us_ = now;

    probes_.poll();

    // A hidden trace must not reach a legend aggregate. The Graphs and
    // Telemetry filters both hide series, so without this a row could report
    // "max 3.5 W" from a shunt whose cell and trace are both off screen. The
    // graph's own visibility is the single source of truth, so this answers for
    // every filter at once; a null graph (a section not built on this host)
    // counts everything.
    auto counted = [](GraphArea* g, int k) {
        return !g || g->series_visible(k);
    };

    const auto& tv = probes_.temp_values();
    if (!tv.empty() &&
        static_cast<int>(tv.size()) == temp_graph_->series_count()) {
        temp_graph_->push(tv);
        for (size_t i = 0; i < temp_values_.size() && i < tv.size(); ++i)
            temp_values_[i]->set_text(fmt_temp(tv[i]));
        for (const auto& a : temp_agg_labels_) {
            // MAX, not mean: a card's sensors sit on different dies and the
            // hottest one is what throttles or trips. Averaging four sensors
            // buries a single die running 20 C above its neighbours, which is
            // exactly the case the row exists to surface. Power's row already
            // uses max for the same reason.
            double hottest = 0.0;
            int cnt = 0;
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(tv.size()); ++k) {
                if (!counted(temp_graph_, k)) continue;
                if (!std::isnan(tv[k])) {
                    if (!cnt || tv[k] > hottest) hottest = tv[k];
                    ++cnt;
                }
            }
            a.label->set_text(cnt ? "max " + fmt_temp(hottest) : "max —");
        }
    }

    const auto& fv = probes_.freq_values();
    if (freq_graph_ && !fv.empty() &&
        static_cast<int>(fv.size()) == freq_graph_->series_count()) {
        freq_graph_->push(fv);
        for (size_t i = 0; i < freq_values_.size() && i < fv.size(); ++i)
            freq_values_[i]->set_text(fmt_freq(fv[i]));
        for (const auto& a : freq_agg_labels_) {
            // Mean here, unlike temperature: throttling moves every chip of a
            // card together, so the average reads as "the card's clock".
            double sum = 0.0;
            int cnt = 0;
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(fv.size()); ++k) {
                if (!counted(freq_graph_, k)) continue;
                if (!std::isnan(fv[k])) { sum += fv[k]; ++cnt; }
            }
            a.label->set_text(cnt ? "avg " + fmt_freq(sum / cnt) : "avg —");
        }
    }

    const auto& pv = probes_.power_values();
    if (!pv.empty() &&
        static_cast<int>(pv.size()) == power_graph_->series_count()) {
        power_graph_->push(pv);
        for (size_t i = 0; i < power_values_.size() && i < pv.size(); ++i)
            power_values_[i]->set_text(fmt_power(pv[i]));
        for (const auto& a : power_max_labels_) {
            double best = std::nan("");
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(pv.size()); ++k) {
                if (!counted(power_graph_, k)) continue;
                if (!std::isnan(pv[k]) && (std::isnan(best) || pv[k] > best))
                    best = pv[k];
            }
            a.label->set_text(std::isnan(best) ? "max —"
                                               : "max " + fmt_power(best));
        }
    }

    // Accumulated energy. The row aggregate is a SUM, not power's max or
    // temperature's mean: these are additive, so a card whose rail is split
    // across two shunts should report the total it drew.
    const auto& jv = probes_.energy_values();
    if (accum_graph_ && !jv.empty() &&
        static_cast<int>(jv.size()) == accum_graph_->series_count()) {
        accum_graph_->push(jv);
        for (size_t i = 0; i < accum_values_.size() && i < jv.size(); ++i)
            accum_values_[i]->set_text(fmt_joules(jv[i]));
        for (const auto& a : accum_sum_labels_) {
            double sum = 0.0;
            int cnt = 0;
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(jv.size()); ++k) {
                if (!counted(accum_graph_, k)) continue;
                if (!std::isnan(jv[k])) { sum += jv[k]; ++cnt; }
            }
            a.label->set_text(cnt ? "total " + fmt_joules(sum) : "total —");
        }
    }

    // System rails from the inline supply meter. Same aggregate choices as the
    // accelerator ones below: min for voltage (a brown-out is the lowest
    // excursion), peak-by-magnitude for current, max for power.
    const auto& svv = probes_.sysvoltage_values();
    if (sysvbus_graph_ && !svv.empty() &&
        static_cast<int>(svv.size()) == sysvbus_graph_->series_count()) {
        sysvbus_graph_->push(svv);
        for (size_t i = 0; i < sysvbus_values_.size() && i < svv.size(); ++i)
            sysvbus_values_[i]->set_text(fmt_volts(svv[i]));
        for (const auto& a : sysvbus_min_labels_) {
            double lo = std::numeric_limits<double>::infinity();
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(svv.size()); ++k)
                if (!std::isnan(svv[k]) && svv[k] < lo) lo = svv[k];
            a.label->set_text(std::isinf(lo) ? "min —" : "min " + fmt_volts(lo));
        }
    }

    const auto& scv = probes_.syscurrent_values();
    if (syscurr_graph_ && !scv.empty() &&
        static_cast<int>(scv.size()) == syscurr_graph_->series_count()) {
        syscurr_graph_->push(scv);
        for (size_t i = 0; i < syscurr_values_.size() && i < scv.size(); ++i)
            syscurr_values_[i]->set_text(fmt_amps(scv[i]));
        for (const auto& a : syscurr_absmax_labels_) {
            double peak = 0.0; bool any = false;
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(scv.size()); ++k) {
                if (!counted(syscurr_graph_, k)) continue;
                if (!counted(sysvbus_graph_, k)) continue;
                if (std::isnan(scv[k])) continue;
                if (!any || std::fabs(scv[k]) > std::fabs(peak)) peak = scv[k];
                any = true;
            }
            a.label->set_text(any ? "peak " + fmt_amps(peak) : "peak —");
        }
    }

    const auto& spv = probes_.syspower_values();
    if (syspower_graph_ && !spv.empty() &&
        static_cast<int>(spv.size()) == syspower_graph_->series_count()) {
        syspower_graph_->push(spv);
        for (size_t i = 0; i < syspower_values_.size() && i < spv.size(); ++i)
            syspower_values_[i]->set_text(fmt_power(spv[i]));
        for (const auto& a : syspower_max_labels_) {
            double hi = -std::numeric_limits<double>::infinity();
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(spv.size()); ++k)
                if (!std::isnan(spv[k]) && spv[k] > hi) hi = spv[k];
            a.label->set_text(std::isinf(hi) ? "max —" : "max " + fmt_power(hi));
        }
    }

    // Bus voltage. The row aggregate is the MINIMUM, not power's max or
    // temperature's mean: a supply problem shows up as the lowest excursion,
    // and averaging it away is exactly how a brown-out stays invisible.
    const auto& uv = probes_.voltage_values();
    if (vbus_graph_ && !uv.empty() &&
        static_cast<int>(uv.size()) == vbus_graph_->series_count()) {
        vbus_graph_->push(uv);
        for (size_t i = 0; i < vbus_values_.size() && i < uv.size(); ++i)
            vbus_values_[i]->set_text(fmt_volts(uv[i]));
        for (const auto& a : vbus_min_labels_) {
            double lo = std::numeric_limits<double>::infinity();
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(uv.size()); ++k) {
                if (!counted(vbus_graph_, k)) continue;
                if (!counted(syspower_graph_, k)) continue;
                if (!std::isnan(uv[k]) && uv[k] < lo) lo = uv[k];
            }
            a.label->set_text(std::isinf(lo) ? "min —" : "min " + fmt_volts(lo));
        }
    }

    // Current. The row aggregate is the PEAK BY MAGNITUDE, shown with its sign:
    // draw reads negative on this rig, so a plain max would report the quietest
    // moment and a plain min would be right only by accident of the wiring.
    const auto& av = probes_.current_values();
    if (curr_graph_ && !av.empty() &&
        static_cast<int>(av.size()) == curr_graph_->series_count()) {
        curr_graph_->push(av);
        for (size_t i = 0; i < curr_values_.size() && i < av.size(); ++i)
            curr_values_[i]->set_text(fmt_amps(av[i]));
        for (const auto& a : curr_absmax_labels_) {
            double peak = 0.0; bool any = false;
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(av.size()); ++k) {
                if (!counted(curr_graph_, k)) continue;
                if (std::isnan(av[k])) continue;
                if (!any || std::fabs(av[k]) > std::fabs(peak)) peak = av[k];
                any = true;
            }
            a.label->set_text(any ? "peak " + fmt_amps(peak) : "peak —");
        }
    }

    return true;
}

void MainWindow::on_about() {
    // Configure once; thereafter just re-show the same (hidden) dialog.
    if (!about_ready_) {
        about_ready_ = true;
        about_dialog_.add_css_class("mb-about");
        about_dialog_.set_transient_for(*this);
        about_dialog_.set_modal(true);
        about_dialog_.set_hide_on_close(true);
        about_dialog_.set_program_name(
            "NPU Power and Temperature Monitoring GUI");
        about_dialog_.set_version("0.01");
        about_dialog_.set_comments(
            "Monitors the power and temperature of edge-AI NPUs.");
        about_dialog_.set_copyright("© 2026 Mario Bergeron");
        about_dialog_.set_license_type(Gtk::License::APACHE_2_0);
        about_dialog_.set_website("https://mariobergeron.com");
        about_dialog_.set_website_label("mariobergeron.com");
        try {
            about_dialog_.set_logo(Gdk::Texture::create_from_resource(
                "/com/mariobergeron/mbpowermon/M_benchmarking.png"));
        } catch (const Glib::Error& e) {
            g_warning("about logo: %s", e.what());
        }
    }
    about_dialog_.set_visible(true);
}
