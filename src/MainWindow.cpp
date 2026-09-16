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
#include <gtkmm/stylecontext.h>

#include <cmath>
#include <cstdio>

#include "util.h"

namespace {
constexpr int kIntervalMs = 1000;
constexpr int kSpanSeconds = 600;  // 10 min of history on every graph
constexpr int kHistory = kSpanSeconds + 1;
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
    set_default_size(940, 620);

    // Teal title bar (brand Primary), white title text.
    auto* header = Gtk::make_managed<Gtk::HeaderBar>();
    auto* title = Gtk::make_managed<Gtk::Label>("NPU Power and Temperature Monitoring GUI");
    title->add_css_class("title");
    header->set_title_widget(*title);

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

    // Fill layout (not a natural-height scroller): the two sections share the
    // window's vertical space so the graphs grow with the window.
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    root->set_margin(12);
    set_child(*root);

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
                                 &sysvbus_min_labels_,
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
                                 &syscurr_absmax_labels_,
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
                                 &syspower_max_labels_,
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
                                 &vbus_min_labels_,
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
                                 &curr_absmax_labels_,
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
                             &power_max_labels_)));

    // Accumulated energy sits with Power because it is its integral.
    if (!probes_.energy_metrics().empty()) {
        root->append(make_section(
            "Accumulated Energy (J)",
            build_metric_section(probes_.energy_metrics(),
                                 colors_for(probes_.energy_metrics()),
                                 /*percent_temp_axis=*/false, fmt_joules,
                                 accum_graph_, accum_values_,
                                 "No INA228 shunts, so nothing accumulates.",
                                 &accum_sum_labels_,
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
                             &temp_agg_labels_)));

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
                             &freq_agg_labels_, /*min_axis_max=*/1000.0),
        /*expanded=*/false));

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
    const char* empty_note, std::vector<AggEntry>* agg_out, double min_axis_max) {
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
            ++col;
        }

        while (i < n && metrics[i].device == dev) {
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
            auto* name = Gtk::make_managed<Gtk::Label>(lbl);
            name->set_xalign(0.0);
            name->set_width_chars(4);
            cell->append(*name);

            auto* val = Gtk::make_managed<Gtk::Label>("—");
            val->set_xalign(1.0);
            val->set_width_chars(6);
            value_labels_out[i] = val;
            cell->append(*val);

            grid->attach(*cell, col, row, 1, 1);
            ++col;
            ++i;
        }
        if (agg_out) agg_out->push_back({agg_label, agg_start, i - agg_start});
        ++row;
    }
    box->append(*grid);
    return *box;
}

bool MainWindow::on_tick() {
    const std::int64_t now = g_get_monotonic_time();
    (void)now;
    last_time_us_ = now;

    probes_.poll();

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
