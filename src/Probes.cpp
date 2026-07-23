#include "Probes.h"

#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <hailo/hailort.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace {

const double kNaN = std::nan("");

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

std::vector<std::string> glob_paths(const std::string& pattern) {
    std::vector<std::string> out;
    glob_t g{};
    if (glob(pattern.c_str(), 0, nullptr, &g) == 0)
        for (size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    globfree(&g);
    return out;
}

std::string basename_of(const std::string& p) {
    auto s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Full PCIe BDF (e.g. "0000:47:00.0") of the first device with this vendor id.
std::string find_pci_bdf_by_vendor(unsigned vendor) {
    for (const auto& d : glob_paths("/sys/bus/pci/devices/*")) {
        std::string v = read_file(d + "/vendor");
        if (static_cast<unsigned>(std::strtoul(v.c_str(), nullptr, 16)) == vendor)
            return basename_of(d);
    }
    return {};
}

// Run a shell command and capture stdout. Commands are built from trusted
// binary paths + device nodes discovered from the filesystem.
std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

// ---------------------------------------------------------------------------
// Hailo-8 — TS0/TS1 + firmware-averaged power via the HailoRT C++ runtime API.
// Temperature reads are passive; the power session is *not* — it uses the
// shared firmware averaging buffer (and disables OCP while active), so it can
// interfere with, or be clobbered by, another HailoRT power client. Included
// here at the user's request. Auto-recovers the buffer after repeated misses.
// ---------------------------------------------------------------------------
class HailoProbe : public DeviceProbe {
public:
    const char* name() const override { return "Hailo"; }

    bool discover() {
        auto ids = hailort::Device::scan();
        if (!ids || ids->empty()) return false;
        bdf_ = ids->at(0);  // HailoRT device ids are the PCIe BDF
        auto dev = hailort::Device::create(ids->at(0));
        if (!dev) return false;
        dev_ = dev.release();

        temp_metrics_.push_back({"Hailo TS0", "°C"});
        temp_metrics_.push_back({"Hailo TS1", "°C"});
        temp_values_.assign(2, kNaN);

        if (start_power()) {
            has_power_ = true;
            power_metrics_.push_back({"Hailo POW", "W"});
            power_values_.assign(1, kNaN);
        }
        return true;
    }

    void poll() override {
        auto t = dev_->get_chip_temperature();
        if (t) {
            temp_values_[0] = t.value().ts0_temperature;
            temp_values_[1] = t.value().ts1_temperature;
        } else {
            temp_values_[0] = temp_values_[1] = kNaN;
        }
        if (has_power_) {
            auto p = dev_->get_power_measurement(HAILO_MEASUREMENT_BUFFER_INDEX_0,
                                                 true);
            if (p) {
                power_values_[0] = p.value().average_value;
                power_fail_ = 0;
            } else {
                power_values_[0] = kNaN;
                if (++power_fail_ >= 3) {  // another client clobbered us
                    start_power();
                    power_fail_ = 0;
                }
            }
        }
    }

    ~HailoProbe() override {
        if (dev_ && has_power_) dev_->stop_power_measurement();
    }

private:
    bool start_power() {
        // The firmware runs its own periodic sampling on the OCP DVM by
        // default; stop it first (ignoring errors) so our session can claim the
        // buffer, then take it over. This also recovers a session another
        // HailoRT client clobbered.
        dev_->stop_power_measurement();
        if (dev_->set_power_measurement(HAILO_MEASUREMENT_BUFFER_INDEX_0,
                                        HAILO_DVM_OPTIONS_AUTO,
                                        HAILO_POWER_MEASUREMENT_TYPES__POWER) !=
            HAILO_SUCCESS)
            return false;
        return dev_->start_power_measurement(HAILO_AVERAGE_FACTOR_256,
                                             HAILO_SAMPLING_PERIOD_1100US) ==
               HAILO_SUCCESS;
    }

    std::unique_ptr<hailort::Device> dev_;
    bool has_power_ = false;
    int power_fail_ = 0;
};

// ---------------------------------------------------------------------------
// DeepX M1 — `dxrt-cli -s`, per-NPU temperature lines (reads the kernel driver;
// works even with the runtime daemon stopped, never claims the device).
// ---------------------------------------------------------------------------
class DeepXProbe : public DeviceProbe {
public:
    const char* name() const override { return "DeepX"; }

    bool discover() {
        for (const char* c : {"/usr/local/bin/dxrt-cli", "dxrt-cli"}) {
            if (std::string(c)[0] != '/' || file_exists(c)) { cli_ = c; break; }
        }
        if (cli_.empty()) cli_ = "dxrt-cli";
        auto temps = read_temps();
        if (temps.empty()) return false;
        bdf_ = find_pci_bdf_by_vendor(0x1ff4);  // DeepX
        for (size_t i = 0; i < temps.size(); ++i) {
            temp_metrics_.push_back({"DeepX T" + std::to_string(i), "°C"});
            temp_values_.push_back(temps[i]);
        }
        return true;
    }

    void poll() override {
        auto temps = read_temps();
        for (size_t i = 0; i < temp_values_.size(); ++i)
            temp_values_[i] = (i < temps.size()) ? temps[i] : kNaN;
    }

private:
    std::vector<double> read_temps() {
        std::string out = run_capture("timeout 5 " + cli_ + " -s 2>/dev/null");
        static const std::regex re(
            R"(NPU\s+(\d+)\s*:.*?temperature\s+([\d.]+)\s*'?\s*C)");
        std::map<int, double> by_idx;
        std::istringstream is(out);
        std::string line;
        while (std::getline(is, line)) {
            std::smatch m;
            if (std::regex_search(line, m, re))
                by_idx[std::stoi(m[1])] = std::stod(m[2]);
        }
        std::vector<double> v;
        for (auto& [idx, t] : by_idx) { (void)idx; v.push_back(t); }
        return v;
    }

    std::string cli_;
};

// ---------------------------------------------------------------------------
// MemryX MX3 — per-MPU temperature from sysfs/hwmon (pure kernel reads;
// the device is shared through the mxa-manager daemon regardless).
// ---------------------------------------------------------------------------
class MemryXProbe : public DeviceProbe {
public:
    const char* name() const override { return "MemryX"; }

    bool discover() {
        for (const auto& d : glob_paths("/sys/class/hwmon/hwmon*")) {
            std::string nm = read_file(d + "/name");
            while (!nm.empty() && (nm.back() == '\n' || nm.back() == ' '))
                nm.pop_back();
            if (nm != "memx0") continue;
            hwmon_ = d;
            break;
        }
        if (hwmon_.empty()) return false;
        for (int i = 1; i <= 16; ++i) {
            std::string raw = read_file(hwmon_ + "/temp" + std::to_string(i) +
                                        "_input");
            if (raw.find_first_of("0123456789") == std::string::npos) continue;
            slots_.push_back(i);
        }
        if (slots_.empty()) return false;
        bdf_ = find_pci_bdf_by_vendor(0x1fe9);  // MemryX
        for (size_t i = 0; i < slots_.size(); ++i) {
            temp_metrics_.push_back({"MemryX T" + std::to_string(i), "°C"});
            temp_values_.push_back(kNaN);
        }

        // Power comes only from the MemryX SDK, which lives in a Python venv.
        // Python import is ~4 s, so a per-tick shell-out is impossible; instead
        // run a persistent helper that connects once to the mxa-manager daemon
        // (multi-process-safe) and streams power once per second.
        if (start_power_helper()) {
            power_metrics_.push_back({"MemryX POW", "W"});
            power_values_.assign(1, kNaN);
        }

        poll();
        return true;
    }

    void poll() override {
        for (size_t i = 0; i < slots_.size(); ++i) {
            std::string raw = read_file(hwmon_ + "/temp" +
                                        std::to_string(slots_[i]) + "_input");
            try {
                temp_values_[i] = raw.empty() ? kNaN : std::stod(raw) / 1000.0;
            } catch (...) {
                temp_values_[i] = kNaN;
            }
        }
        if (power_fd_ >= 0) {
            drain_power_helper();
            power_values_[0] = last_power_;
        }
    }

    ~MemryXProbe() override {
        if (helper_pid_ > 0) {
            kill(helper_pid_, SIGTERM);
            waitpid(helper_pid_, nullptr, 0);
        }
        if (power_fd_ >= 0) close(power_fd_);
    }

private:
    // Locate a Python interpreter whose venv actually has the `memryx` package.
    static std::string find_memryx_python() {
        if (const char* env = std::getenv("MB_MEMRYX_PYTHON"))
            if (file_exists(env)) return env;
        std::vector<std::string> venvs;
        if (const char* home = std::getenv("HOME"))
            venvs.push_back(std::string(home) + "/mb-edgeai/memryx-env");
        for (const auto& v : venvs) {
            if (glob_paths(v + "/lib/python*/site-packages/memryx").empty())
                continue;
            std::string py = v + "/bin/python3";
            if (file_exists(py)) return py;
        }
        return {};
    }

    bool start_power_helper() {
        std::string py = find_memryx_python();
        if (py.empty()) return false;
        static const char* kScript =
            "import sys,time\n"
            "try:\n"
            " from memryx import mxa\n"
            "except Exception:\n"
            " sys.exit(3)\n"
            "while True:\n"
            " try: w=mxa.get_power(0)/1000.0\n"
            " except Exception: w=float('nan')\n"
            " sys.stdout.write('%.4f\\n'%w); sys.stdout.flush()\n"
            " time.sleep(1)\n";
        int fds[2];
        if (pipe(fds) != 0) return false;
        pid_t pid = fork();
        if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
        if (pid == 0) {  // child
            dup2(fds[1], STDOUT_FILENO);
            close(fds[0]);
            close(fds[1]);
            execl(py.c_str(), py.c_str(), "-u", "-c", kScript,
                  static_cast<char*>(nullptr));
            _exit(127);
        }
        close(fds[1]);
        fcntl(fds[0], F_SETFL, O_NONBLOCK);
        power_fd_ = fds[0];
        helper_pid_ = pid;
        return true;
    }

    void drain_power_helper() {
        char tmp[4096];
        ssize_t n;
        while ((n = read(power_fd_, tmp, sizeof(tmp))) > 0)
            pbuf_.append(tmp, n);
        size_t nl = pbuf_.rfind('\n');
        if (nl == std::string::npos) return;
        std::string complete = pbuf_.substr(0, nl);
        pbuf_.erase(0, nl + 1);
        size_t last = complete.rfind('\n');
        std::string line =
            (last == std::string::npos) ? complete : complete.substr(last + 1);
        try {
            last_power_ = std::stod(line);
        } catch (...) {
        }
    }

    std::string hwmon_;
    std::vector<int> slots_;
    int power_fd_ = -1;
    pid_t helper_pid_ = -1;
    double last_power_ = kNaN;
    std::string pbuf_;
};

// ---------------------------------------------------------------------------
// Axelera Metis — per-core temps from the `triton_trace` collector log. We only
// *peek* (never enable the collector or open a Context), so we never race for
// device ownership. If the collector isn't already running the temps read as
// missing — a deliberately passive tradeoff.
// ---------------------------------------------------------------------------
class AxeleraProbe : public DeviceProbe {
public:
    const char* name() const override { return "Axelera"; }

    bool discover() {
        auto nodes = glob_paths("/dev/metis-0:*");
        if (nodes.empty()) return false;
        device_ = basename_of(nodes.front());  // e.g. "metis-0:c2:0"

        auto bins = glob_paths("/opt/axelera/runtime-*/bin/triton_trace");
        cli_ = bins.empty() ? "triton_trace" : bins.front();

        auto temps = read_temps();
        if (temps.empty()) return false;
        bdf_ = find_pci_bdf_by_vendor(0x1f9d);  // Axelera
        static const char* kLabels[] = {"SYS", "AI0", "AI1", "AI2", "AI3"};
        for (size_t i = 0; i < temps.size() && i < 5; ++i) {
            temp_metrics_.push_back(
                {std::string("Axelera ") + kLabels[i], "°C"});
            temp_values_.push_back(temps[i]);
        }
        return true;
    }

    void poll() override {
        auto temps = read_temps();
        for (size_t i = 0; i < temp_values_.size(); ++i)
            temp_values_[i] = (i < temps.size()) ? temps[i] : kNaN;
    }

private:
    std::vector<double> read_temps() {
        std::string out = run_capture("timeout 4 " + cli_ + " --device " +
                                      device_ + " --slog --peek 2>/dev/null");
        static const std::regex re(R"(core_temps=\[([0-9,\s]+)\])");
        std::vector<double> last;
        for (std::sregex_iterator it(out.begin(), out.end(), re), end;
             it != end; ++it) {
            std::vector<double> cur;
            std::stringstream ss((*it)[1].str());
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                try { cur.push_back(std::stod(tok)); } catch (...) {}
            }
            if (!cur.empty()) last = std::move(cur);
        }
        return last;
    }

    std::string cli_, device_;
};

}  // namespace

void Probes::discover(std::vector<std::string>* notes) {
    auto try_add = [&](std::unique_ptr<DeviceProbe> p, bool ok) {
        if (ok) {
            if (notes)
                notes->push_back(std::string(p->name()) + ": " +
                                 std::to_string(p->temp_metrics().size()) +
                                 " temp sensor(s)");
            devices_.push_back(std::move(p));
        } else if (notes) {
            notes->push_back(std::string(p->name()) + ": not present / no data");
        }
    };

    {
        auto p = std::make_unique<HailoProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
    {
        auto p = std::make_unique<DeepXProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
    {
        auto p = std::make_unique<MemryXProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
    {
        auto p = std::make_unique<AxeleraProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }

    flatten();
}

void Probes::flatten() {
    temp_metrics_.clear();
    power_metrics_.clear();
    for (size_t k = 0; k < devices_.size(); ++k) {
        auto& d = devices_[k];
        for (auto m : d->temp_metrics()) {
            m.device = static_cast<int>(k);
            m.device_name = d->name();
            m.bdf = d->bdf();
            temp_metrics_.push_back(std::move(m));
        }
        for (auto m : d->power_metrics()) {
            m.device = static_cast<int>(k);
            m.device_name = d->name();
            m.bdf = d->bdf();
            power_metrics_.push_back(std::move(m));
        }
    }
    temp_values_.assign(temp_metrics_.size(), kNaN);
    power_values_.assign(power_metrics_.size(), kNaN);
}

void Probes::poll() {
    for (auto& d : devices_) d->poll();
    size_t ti = 0, pi = 0;
    for (auto& d : devices_) {
        for (double v : d->temp_values()) temp_values_[ti++] = v;
        for (double v : d->power_values()) power_values_[pi++] = v;
    }
}
