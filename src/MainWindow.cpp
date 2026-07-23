#include "MainWindow.h"

#include <glib.h>
#include <glibmm/main.h>
#include <gdkmm/display.h>
#include <gtkmm/box.h>
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
constexpr int kSpanSeconds = 60;
constexpr int kHistory = kSpanSeconds + 1;
constexpr double kTempAxisMax = 100.0;  // °C, matches mb-powermon's default

std::string fmt_temp(double v) {
    if (std::isnan(v)) return "—";
    char b[24];
    std::snprintf(b, sizeof(b), "%.0f°C", v);
    return b;
}
std::string fmt_power(double v) {
    if (std::isnan(v)) return "—";
    char b[24];
    std::snprintf(b, sizeof(b), "%.2f W", v);
    return b;
}
std::string fmt_temp_axis(double v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%.0f°C", v);
    return b;
}
}  // namespace

MainWindow::MainWindow() {
    set_title("mb-powermon-gui - Power Monitor GUI");
    set_default_size(940, 620);

    // Teal title bar (brand Primary), white title text.
    auto* header = Gtk::make_managed<Gtk::HeaderBar>();
    auto* title = Gtk::make_managed<Gtk::Label>("mb-powermon-gui - Power Monitor GUI");
    title->add_css_class("title");
    header->set_title_widget(*title);
    set_titlebar(*header);

    auto css = Gtk::CssProvider::create();
    css->load_from_data(
        "headerbar { background: #64A19D; box-shadow: none; }"
        "headerbar label.title { color: #FFFFFF; font-weight: bold; }");
    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(), css,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    std::vector<std::string> notes;
    probes_.discover(&notes);
    for (const auto& n : notes) g_message("probe: %s", n.c_str());

    device_palette_ = util::make_palette(std::max(1, probes_.device_count()));
    last_time_us_ = g_get_monotonic_time();

    // Fill layout (not a natural-height scroller): the two sections share the
    // window's vertical space so the graphs grow with the window.
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    root->set_margin(12);
    set_child(*root);

    root->append(make_section(
        "Power",
        build_metric_section(probes_.power_metrics(),
                             colors_for(probes_.power_metrics()),
                             /*percent_temp_axis=*/false, fmt_power, power_graph_,
                             power_values_,
                             "No power source available. On these M.2 cards "
                             "power comes only from the Hailo firmware session "
                             "or the MemryX SDK — or an external meter "
                             "(INA228 / PMD2).")));

    root->append(make_section(
        "Temperature",
        build_metric_section(probes_.temp_metrics(),
                             colors_for(probes_.temp_metrics()),
                             /*percent_temp_axis=*/true, fmt_temp, temp_graph_,
                             temp_values_, "No temperature sensors detected.")));

    Glib::signal_timeout().connect(sigc::mem_fun(*this, &MainWindow::on_tick),
                                   kIntervalMs);
}

Gtk::Expander& MainWindow::make_section(const char* title, Gtk::Widget& content) {
    auto* exp = Gtk::make_managed<Gtk::Expander>();
    auto* lbl = Gtk::make_managed<Gtk::Label>();
    lbl->set_markup(std::string("<b>") + title + "</b>");
    exp->set_label_widget(*lbl);
    exp->set_expanded(true);
    exp->set_margin_top(4);
    exp->set_vexpand(true);  // share vertical space with the other section
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
    const char* empty_note) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_vexpand(true);  // graph inside grows; legend keeps natural height

    const int n = static_cast<int>(metrics.size());

    auto* graph = Gtk::make_managed<GraphArea>(kHistory, kSpanSeconds);
    graph->set_percent_mode(false);
    if (temp_axis) {
        graph->set_fixed_max(kTempAxisMax);
        graph->set_value_formatter(fmt_temp_axis);
    } else {
        graph->set_min_axis_max(10.0);  // default 10 W floor; auto-expands above
        graph->set_value_formatter([](double v) { return fmt_power(v); });
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
    }

    const auto& pv = probes_.power_values();
    if (!pv.empty() &&
        static_cast<int>(pv.size()) == power_graph_->series_count()) {
        power_graph_->push(pv);
        for (size_t i = 0; i < power_values_.size() && i < pv.size(); ++i)
            power_values_[i]->set_text(fmt_power(pv[i]));
    }

    return true;
}
