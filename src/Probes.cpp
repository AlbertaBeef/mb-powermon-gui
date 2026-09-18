#include "Probes.h"

#include <fcntl.h>
#include <glob.h>
#include <termios.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if MB_HAVE_HAILO
#include <hailo/hailort.hpp>
#endif

#if MB_HAVE_FTDI
#include <libftdi1/ftdi.h>
#include <chrono>
#include <thread>
#endif

#include <climits>
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

// A die temperature outside this band is not a measurement, it is a driver
// sentinel. The MX3 publishes 65262000 (= -274 °C read as unsigned 16-bit,
// just below absolute zero) on every sensor once the chip stops answering
// admin commands — `memryx: admin timeout ... subop 17` in dmesg. Reporting
// that verbatim is worse than reporting nothing: since no graph clips its data
// any more, one bogus sample re-tops the temperature axis at ~72000 °C and
// squashes every real card into the bottom pixel row. NaN draws as a gap and
// leaves the axis alone, which is what "this sensor has no reading" should
// look like.
constexpr double kTempMinPlausible = -40.0;
constexpr double kTempMaxPlausible = 150.0;

inline double plausible_temp(double c) {
    return (c >= kTempMinPlausible && c <= kTempMaxPlausible) ? c : kNaN;
}

// Same idea as plausible_temp, and load-bearing for the same reason: vendor
// SDKs return an in-band sentinel instead of failing when a reading is not
// available. A MemryX MX3 without power telemetry answers get_power() with
// 0xFFFFFFFF mW, which becomes 4294967.295 W.
//
// That number is far more damaging than the temperature sentinel, because
// power_for_device() returns the MAX over a card's power metrics and that value
// becomes the engine's watts. NaN is skipped by that max, so a real folded
// INA228 reading still wins — but 4294967.295 is a finite number, so it wins
// instead and every fps/W and mJ/frame figure for that card is wrong by six
// orders of magnitude.
//
// 0 W is deliberately INSIDE the range: it is a real INA228 overflow signal and
// a genuine idle reading, so it must pass through as a value, not become a gap.
// The ceiling is generous on purpose — the largest thing this app legitimately
// measures is whole-board draw through the POWER-Z, ~25 W — so 1000 W cannot
// reject a real reading while still catching a 32-bit sentinel.
//
// Like plausible_temp: map to NaN, never clamp. A fabricated in-range number
// sitting next to genuine readings is worse than an empty field.
constexpr double kPowerMinPlausible = 0.0;
constexpr double kPowerMaxPlausible = 1000.0;

inline double plausible_power(double w) {
    return (w >= kPowerMinPlausible && w <= kPowerMaxPlausible) ? w : kNaN;
}


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

// Strip trailing newline / space / NUL — sysfs strings and device-tree strings
// respectively.
std::string trim_sysfs(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || !s.back()))
        s.pop_back();
    return s;
}

// Short board name for an SoC platform: "Qualcomm Technologies, Inc. Addons IQ
// 9075 EVK" -> "IQ9075". Falls back to the SoC id ("QCS9075"), then to "".
std::string board_name() {
    std::string model = trim_sysfs(read_file("/proc/device-tree/model"));
    std::smatch m;
    static const std::regex re(R"(\b(IQ|QCS|QRB|SA)[- ]?(\d{4}[A-Z]?)\b)");
    if (std::regex_search(model, m, re)) return m[1].str() + m[2].str();
    return trim_sysfs(read_file("/sys/devices/soc0/machine"));
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

#if MB_HAVE_HAILO
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

        // The NN core clock lives in the extended device information — not
        // under any name containing "frequency", which is why a naive grep of
        // the headers suggests HailoRT exposes no clock at all. It does.
        if (dev_->get_extended_device_information()) {
            freq_metrics_.push_back({"Hailo CLK", "MHz"});
            freq_values_.assign(1, kNaN);
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
        if (!freq_values_.empty()) {
            auto x = dev_->get_extended_device_information();
            freq_values_[0] = x ? x->neural_network_core_clock_rate / 1e6 : kNaN;
        }
        if (has_power_) {
            auto p = dev_->get_power_measurement(HAILO_MEASUREMENT_BUFFER_INDEX_0,
                                                 true);
            if (p) {
                power_values_[0] = plausible_power(p.value().average_value);
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
#endif  // MB_HAVE_HAILO

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
        Reading r = read_status();
        if (r.temps.empty()) return false;
        bdf_ = find_pci_bdf_by_vendor(0x1ff4);  // DeepX
        for (size_t i = 0; i < r.temps.size(); ++i) {
            temp_metrics_.push_back({"DeepX T" + std::to_string(i), "°C"});
            temp_values_.push_back(r.temps[i]);
        }
        for (size_t i = 0; i < r.clocks.size(); ++i) {
            freq_metrics_.push_back({"DeepX C" + std::to_string(i), "MHz"});
            freq_values_.push_back(r.clocks[i]);
        }
        return true;
    }

    void poll() override {
        Reading r = read_status();
        for (size_t i = 0; i < temp_values_.size(); ++i)
            temp_values_[i] = (i < r.temps.size()) ? r.temps[i] : kNaN;
        for (size_t i = 0; i < freq_values_.size(); ++i)
            freq_values_[i] = (i < r.clocks.size()) ? r.clocks[i] : kNaN;
    }

private:
    struct Reading {
        std::vector<double> temps;
        std::vector<double> clocks;  // MHz
    };

    // One `dxrt-cli -s` shell-out yields both, from the same line:
    //   NPU 0: voltage 750 mV, clock 1000 MHz, temperature 51'C
    // Parsing them together keeps it to one subprocess per poll.
    Reading read_status() {
        std::string out = run_capture("timeout 5 " + cli_ + " -s 2>/dev/null");
        static const std::regex re_t(
            R"(NPU\s+(\d+)\s*:.*?temperature\s+([\d.]+)\s*'?\s*C)");
        static const std::regex re_c(
            R"(NPU\s+(\d+)\s*:.*?clock\s+([\d.]+)\s*MHz)");
        std::map<int, double> t_by_idx, c_by_idx;
        std::istringstream is(out);
        std::string line;
        while (std::getline(is, line)) {
            std::smatch m;
            if (std::regex_search(line, m, re_t))
                t_by_idx[std::stoi(m[1])] = std::stod(m[2]);
            if (std::regex_search(line, m, re_c))
                c_by_idx[std::stoi(m[1])] = std::stod(m[2]);
        }
        Reading r;
        for (auto& [idx, v] : t_by_idx) { (void)idx; r.temps.push_back(v); }
        for (auto& [idx, v] : c_by_idx) { (void)idx; r.clocks.push_back(v); }
        return r;
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
            // Same helper, same stream — the clocks ride along for free. One
            // per chip, matching the per-chip temperature sensors, so a
            // throttling dip can be read against the die that caused it.
            for (size_t i = 0; i < slots_.size(); ++i) {
                freq_metrics_.push_back({"MemryX C" + std::to_string(i), "MHz"});
                freq_values_.push_back(kNaN);
            }
        }

        poll();
        return true;
    }

    void poll() override {
        for (size_t i = 0; i < slots_.size(); ++i) {
            std::string raw = read_file(hwmon_ + "/temp" +
                                        std::to_string(slots_[i]) + "_input");
            try {
                temp_values_[i] =
                    raw.empty() ? kNaN : plausible_temp(std::stod(raw) / 1000.0);
            } catch (...) {
                temp_values_[i] = kNaN;
            }
        }
        if (power_fd_ >= 0) {
            drain_power_helper();
            power_values_[0] = plausible_power(last_power_);
            for (size_t i = 0; i < freq_values_.size(); ++i)
                freq_values_[i] = i < last_freqs_.size() ? last_freqs_[i] : kNaN;
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
            // Chip count gates the clock loop: the SDK does NOT bounds-check
            // the group index, and asking beyond get_total_chip_count()
            // returns nonsense (2 MHz) rather than failing.
            "try: n=int(mxa.get_total_chip_count(0))\n"
            "except Exception: n=0\n"
            "while True:\n"
            // get_power() does not raise on a part without power telemetry —
            // it returns 0xFFFFFFFF mW, i.e. 4294967.295 W. Gate it here as
            // well as in plausible_power(), so the sentinel never reaches the
            // pipe and a reader of the raw helper output is not misled either.
            // Bound matches kPowerMaxPlausible: 1e6 mW = 1000 W.
            " try:\n"
            "  _w=mxa.get_power(0)\n"
            "  w=(_w/1000.0) if (0<=_w<1e6) else float('nan')\n"
            " except Exception: w=float('nan')\n"
            // get_frequency_effective(dev, group) is the reading that drops
            // under thermal throttling; get_frequency() returns the configured
            // target and would sit flat at 600/850 however hot the part got.
            // Both take (device, group) — a single int raises TypeError.
            " fs=[]\n"
            " for g in range(n):\n"
            "  try: fs.append(float(mxa.get_frequency_effective(0,g)))\n"
            "  except Exception: fs.append(float('nan'))\n"
            " try:\n"
            "  sys.stdout.write(('%.4f'%w)+''.join(' %.1f'%f for f in fs)+'\\n')\n"
            "  sys.stdout.flush()\n"
            " except Exception:\n"
            "  os._exit(0)\n"
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
        // The line is "<watts> <clk0> <clk1> ...". std::stod would silently
        // take only the first field and drop every clock, so parse the stream.
        std::istringstream ls(line);
        double w = kNaN;
        if (!(ls >> w)) return;
        last_power_ = w;
        std::vector<double> fs;
        for (double f; ls >> f;) fs.push_back(f);
        if (!fs.empty()) last_freqs_ = std::move(fs);
    }

    std::string hwmon_;
    std::vector<int> slots_;
    int power_fd_ = -1;
    pid_t helper_pid_ = -1;
    double last_power_ = kNaN;
    std::vector<double> last_freqs_;   // one effective clock per chip
    std::string pbuf_;
};

// ---------------------------------------------------------------------------
// Axelera Metis — per-core temps from the `triton_trace` collector log. We only
// *peek* (never enable the collector or open a Context), so we never race for
// device ownership.
//
// Presence is keyed on the /dev/metis-0:* node, *not* on temps being available:
// the card is registered (with its standard SYS/AI0–AI3 sensor set, NaN until
// data flows) as soon as the node and a `triton_trace` binary exist. This keeps
// a present-but-silent Metis visible in the UI instead of vanishing, and lets us
// name *why* it is silent — the peek can come back empty for several distinct
// reasons, each surfaced through note_:
//   - firmware/runtime version mismatch (the tool refuses to talk to the card),
//   - the collector simply isn't running (nothing is using the device),
//   - no `triton_trace` on the host at all.
// ---------------------------------------------------------------------------
class AxeleraProbe : public DeviceProbe {
public:
    const char* name() const override { return "Axelera"; }

    bool discover() {
        auto nodes = glob_paths("/dev/metis-0:*");
        if (nodes.empty()) return false;  // genuinely no Metis on this host
        device_ = basename_of(nodes.front());  // e.g. "metis-0:c6:0"

        auto bins = glob_paths("/opt/axelera/runtime-*/bin/triton_trace");
        if (!bins.empty())
            cli_ = bins.front();
        else if (!run_capture("command -v triton_trace 2>/dev/null").empty())
            cli_ = "triton_trace";  // on PATH
        else
            cli_.clear();  // no telemetry tool — device still registers

        bdf_ = find_pci_bdf_by_vendor(0x1f9d);  // Axelera

        // Fixed sensor set for metis-0: SYS + four AI cores. Values arrive (or
        // don't) via poll(); a silent card shows NaN, not absence.
        static const char* kLabels[] = {"SYS", "AI0", "AI1", "AI2", "AI3"};
        for (auto* l : kLabels) {
            temp_metrics_.push_back({std::string("Axelera ") + l, "°C"});
            temp_values_.push_back(kNaN);
        }

        // Per-AI-core clock from `axcmd --clock-all-actual`. Use the ACTUAL
        // reading, never `--get-ck-profile`, which returns the configured
        // profile and sits flat however hot the part gets. One series per
        // core, so a clock reads against the AI0-AI3 temperature beside it.
        auto axcmds = glob_paths("/opt/axelera/runtime-*/bin/axcmd");
        if (!axcmds.empty()) {
            axcmd_ = axcmds.front();
            for (int i = 0; i < 4; ++i) {
                freq_metrics_.push_back({"Axelera C" + std::to_string(i), "MHz"});
                freq_values_.push_back(kNaN);
            }
        }

        poll();  // seed values + set note_ (version mismatch / idle / no tool)
        return true;
    }

    void poll() override {
        auto temps = read_temps();
        for (size_t i = 0; i < temp_values_.size(); ++i)
            temp_values_[i] = (i < temps.size()) ? temps[i] : kNaN;
        if (!freq_values_.empty()) {
            auto clocks = read_clocks();
            for (size_t i = 0; i < freq_values_.size(); ++i)
                freq_values_[i] = (i < clocks.size()) ? clocks[i] : kNaN;
        }
    }

private:
    // "aicore0: 800MHz" -> 800, indexed by core.
    std::vector<double> read_clocks() {
        std::string out = run_capture("timeout 4 " + axcmd_ + " --device " +
                                      device_ + " --clock-all-actual 2>/dev/null");
        static const std::regex re(R"(aicore(\d+)\s*:\s*([\d.]+)\s*MHz)");
        std::map<int, double> by_idx;
        std::istringstream is(out);
        std::string line;
        while (std::getline(is, line)) {
            std::smatch m;
            if (std::regex_search(line, m, re))
                by_idx[std::stoi(m[1])] = std::stod(m[2]);
        }
        std::vector<double> v;
        for (auto& [idx, f] : by_idx) { (void)idx; v.push_back(f); }
        return v;
    }

    std::vector<double> read_temps() {
        if (cli_.empty()) {
            note_ = "triton_trace not found — install the Axelera runtime";
            return {};
        }
        // Capture stderr too (2>&1): the version-mismatch banner prints there.
        std::string out = run_capture("timeout 4 " + cli_ + " --device " +
                                      device_ + " --slog --peek 2>&1");
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

        if (!last.empty()) {
            note_.clear();  // nominal
        } else {
            // Name the reason for the empty read so the UI/log isn't cryptic.
            std::smatch m;
            static const std::regex vm(
                R"RX(Actual="([^"]+)"\s+Expected="([^"]+)")RX");
            if (std::regex_search(out, m, vm))
                note_ = "device firmware " + m[1].str() + " vs tool " +
                        m[2].str() +
                        " — version mismatch, temps unavailable "
                        "(align firmware/runtime)";
            else
                note_ = "collector idle — temps appear once a Metis "
                        "app/collector is running";
        }
        return last;
    }

    std::string cli_, device_;
    std::string axcmd_;   // empty when the runtime's axcmd is not installed
};

// ---------------------------------------------------------------------------
// Qualcomm Dragonwing IQ (IQ-9075 EVK / QCS9075 and relatives) — the NPU here is
// the SoC's own Hexagon NSP, not an M.2 card, so there is no PCIe device and no
// vendor runtime to ask. Temperatures come straight from the kernel's TSENS
// thermal zones (`nsp-<instance>-<block>-<sensor>-thermal`), which is as passive
// as telemetry gets.
//
// Zone naming on this part is nsp-A-B-C: A = NSP instance, B = block within it,
// C = one of two redundant TSENS taps on the same block (they track within a
// degree of each other). We collapse each A/B pair to its **max** and expose one
// metric per block, so a 2x3x2 = 12-zone SoC reads as 6 legend entries.
//
// No power: this board exposes no current sensing anywhere in sysfs — no hwmon
// power*/curr* input, no shunt monitor in the device tree, no powercap/RAPL, and
// the PMIC VADC offers die temperatures and vph_pwr voltage but no current. The
// NSP also shares the package rail with CPU/GPU, so even a package-level number
// would not be an NPU number. Power for this device needs external
// instrumentation (INA228 / PMD2), which this probe deliberately does not fake.
// ---------------------------------------------------------------------------
class QualcommIQProbe : public DeviceProbe {
public:
    const char* name() const override { return name_.c_str(); }

    bool discover() {
        // Group the nsp-A-B-C zones by (instance, block); C is a redundant tap.
        static const std::regex re(R"(^nsp-(\d+)-(\d+)-(\d+)-thermal$)");
        std::map<std::pair<int, int>, std::vector<std::string>> blocks;
        for (const auto& z : glob_paths("/sys/class/thermal/thermal_zone*")) {
            std::string type = trim_sysfs(read_file(z + "/type"));
            std::smatch m;
            if (!std::regex_match(type, m, re)) continue;
            blocks[{std::stoi(m[1]), std::stoi(m[2])}].push_back(z + "/temp");
        }
        if (blocks.empty()) return false;

        name_ = board_name();
        if (name_.empty()) name_ = "Qualcomm";
        bdf_ = trim_sysfs(read_file("/sys/devices/soc0/machine"));  // "QCS9075"

        for (auto& [key, paths] : blocks) {
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "N%d-%d", key.first, key.second);
            temp_metrics_.push_back({name_ + " " + lbl, "°C"});
            zones_.push_back(std::move(paths));
        }
        temp_values_.assign(temp_metrics_.size(), kNaN);
        poll();
        return true;
    }

    void poll() override {
        for (size_t i = 0; i < zones_.size(); ++i) {
            double best = kNaN;
            for (const auto& p : zones_[i]) {
                std::string raw = read_file(p);
                if (raw.empty()) continue;
                try {
                    // Gate before the max: a sentinel would otherwise always
                    // win and become this zone's reported temperature.
                    double c = plausible_temp(std::stod(raw) / 1000.0);
                    if (std::isnan(c)) continue;
                    if (std::isnan(best) || c > best) best = c;
                } catch (...) {
                }
            }
            temp_values_[i] = best;
        }
    }

private:
    std::string name_ = "Qualcomm";
    std::vector<std::vector<std::string>> zones_;  // per metric: redundant taps
};

// ---------------------------------------------------------------------------
// Board ambient — a TMP401-family remote/local temperature sensor (TI TMP411 on
// the IQ-9075 EVK, i2c-19 0x4c, sharing that bus with the amc6821 fan
// controller). This is *board* temperature, not NPU die temperature, so it is a
// separate DeviceProbe: it earns its own legend row and its own `avg`, rather
// than dragging the NSP average toward ambient.
//
// Only the sensor's **local** channel (temp1 = the chip's own die = board
// ambient) is exposed. TMP411 also has a valid remote-diode channel (temp2,
// ~65 °C here, temp2_fault=0), but what that diode is wired to isn't knowable
// without the board schematic, so it is deliberately left out rather than
// labelled with a guess.
//
// The chip is absent from every IQ-9075 device tree, so nothing binds it
// automatically — it must be instantiated once (see README). Until then this
// probe finds nothing and simply contributes no row. Reads here are plain sysfs;
// the one-time config write belongs to the in-tree tmp401 driver at bind, not to
// us, so per-tick behaviour stays passive.
// ---------------------------------------------------------------------------
class BoardThermalProbe : public DeviceProbe {
public:
    const char* name() const override { return name_.c_str(); }

    bool discover() {
        static const char* kFamily[] = {"tmp411", "tmp401", "tmp431", "tmp432",
                                        "tmp435"};
        for (const auto& d : glob_paths("/sys/class/hwmon/hwmon*")) {
            std::string nm = trim_sysfs(read_file(d + "/name"));
            bool known = false;
            for (const char* f : kFamily) known |= (nm == f);
            if (!known) continue;
            std::string p = d + "/temp1_input";
            if (read_file(p).find_first_of("0123456789") == std::string::npos)
                continue;
            path_ = p;
            // The i2c client this hwmon hangs off is named "<bus>-<addr>", e.g.
            // "19-004c" -> the locator "i2c-19 0x4c" for the legend.
            std::string node = basename_of(realpath_of(d + "/device"));
            unsigned bus = 0, addr = 0;
            if (std::sscanf(node.c_str(), "%u-%x", &bus, &addr) == 2) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "i2c-%u 0x%02x", bus, addr);
                bdf_ = buf;
            }
            break;
        }
        if (path_.empty()) return false;

        std::string board = board_name();
        name_ = board.empty() ? "Board" : board + " Board";
        temp_metrics_.push_back({name_ + " AMB", "°C"});
        temp_values_.assign(1, kNaN);
        poll();
        return true;
    }

    void poll() override {
        std::string raw = read_file(path_);
        try {
            temp_values_[0] =
                raw.empty() ? kNaN : plausible_temp(std::stod(raw) / 1000.0);
        } catch (...) {
            temp_values_[0] = kNaN;
        }
    }

private:
    static std::string realpath_of(const std::string& p) {
        char buf[PATH_MAX];
        const char* r = realpath(p.c_str(), buf);
        return r ? std::string(r) : std::string();
    }

    std::string name_ = "Board";
    std::string path_;
};

// ---------------------------------------------------------------------------
// ChargerLAB POWER-Z KM003C — a USB-C inline power meter on the board's own
// supply. The in-tree `powerz` hwmon driver already binds its vendor interface
// (bInterfaceClass ff), so this is plain sysfs: no libusb, no claim, nothing
// that could fight the meter's own control app. Passive like every other probe
// here.
//
// WHAT IT MEASURES IS THE WHOLE BOARD, NOT AN NPU. It sits in the USB-C supply
// feeding the EVK, so it reads CPU + GPU + NSP + DRAM + peripherals together.
// That is genuinely useful — it is the only real wattage available on the
// IQ-9075, whose SoC exposes none — but it is NOT per-accelerator power, and it
// must not be fed to `power_for_device()` as a card's draw. The device name
// deliberately matches no `accel_name()`, which is what keeps it out: a card
// with no rail of its own keeps reporting NaN and its efficiency figures stay
// 0, rather than silently inheriting a board-level number ~10x its real draw.
//
// Verified on this EVK 2026-09-07 by loading eight CPU cores: 11.3 W idle ->
// 22.5 W under load and back, with VBUS sagging 19.86 -> 19.75 V. So it really
// is this board's supply, and the voltage/current/power ordering of the graphs
// reads as cause then effect on it.
//
// Only VBUS/IBUS/TEMP are exposed. The driver also publishes in1..in5 (CC1,
// CC2, D+, D-, and the meter's internal VDD) — USB-C signalling levels, not
// board power. A 1.6 V CC line sharing the voltage graph with a 19.9 V bus
// would squash the trace that matters, so they are left out on the same
// principle as BoardThermalProbe's unlabelled remote diode.
// ---------------------------------------------------------------------------
class PowerZProbe : public DeviceProbe {
public:
    const char* name() const override { return name_.c_str(); }

    bool discover() {
        for (const auto& d : glob_paths("/sys/class/hwmon/hwmon*")) {
            if (trim_sysfs(read_file(d + "/name")) != "powerz") continue;
            if (read_file(d + "/in0_input").empty()) continue;
            if (read_file(d + "/curr1_input").empty()) continue;
            volt_path_ = d + "/in0_input";
            curr_path_ = d + "/curr1_input";
            if (!read_file(d + "/temp1_input").empty())
                temp_path_ = d + "/temp1_input";
            // Locator: the USB port path, matching the INA228 bridges' "usb
            // <port>" form. Taken from the hwmon's own device path
            // (.../usb3/3-1/3-1.4/3-1.4:1.0/hwmon/hwmonN) rather than by
            // re-globbing, so it is the port this meter is actually on.
            std::string real = realpath_of(d);
            const std::string iface = ":1.0";
            const size_t c = real.find(iface);
            if (c != std::string::npos) {
                const size_t b = real.rfind('/', c);
                if (b != std::string::npos)
                    bdf_ = "usb " + real.substr(b + 1, c - b - 1);
            }
            break;
        }
        if (volt_path_.empty()) return false;

        sysvoltage_metrics_.push_back({name_ + " SYS VBUS", "V"});
        syscurrent_metrics_.push_back({name_ + " SYS CURRENT", "A"});
        syspower_metrics_.push_back({name_ + " SYS POWER", "W"});
        sysvoltage_values_.assign(1, kNaN);
        syscurrent_values_.assign(1, kNaN);
        syspower_values_.assign(1, kNaN);
        if (!temp_path_.empty()) {
            temp_metrics_.push_back({name_ + " TEMP", "°C"});
            temp_values_.assign(1, kNaN);
        }
        poll();
        return true;
    }

    void poll() override {
        const double v = read_scaled(volt_path_, 1000.0);   // mV -> V
        // The KM003C reports IBUS **negative while the sink draws**, which is
        // its orientation convention and not a wiring fault (contrast the
        // INA228 rails, where reversed leads really were the bug and are fixed
        // per-rail in ina228.conf). Negate so a board that is drawing reads
        // positive, the way every other current in this app does.
        const double i = -read_scaled(curr_path_, 1000.0);  // mA -> A
        sysvoltage_values_[0] = v;
        syscurrent_values_[0] = i;
        // Magnitude, matching the INA228 POWER register, which is unsigned by
        // hardware. Draw is unsigned regardless of which way round the meter is
        // installed, so this stays right if someone reverses it.
        syspower_values_[0] = (std::isnan(v) || std::isnan(i))
                                  ? kNaN
                                  : plausible_power(std::fabs(v * i));
        if (!temp_path_.empty())
            temp_values_[0] = plausible_temp(read_scaled(temp_path_, 1000.0));
    }

private:
    static double read_scaled(const std::string& path, double div) {
        const std::string raw = read_file(path);
        if (raw.empty()) return kNaN;
        try {
            return std::stod(raw) / div;
        } catch (...) {
            return kNaN;
        }
    }
    static std::string realpath_of(const std::string& p) {
        char buf[PATH_MAX];
        const char* r = realpath(p.c_str(), buf);
        return r ? std::string(r) : std::string();
    }

    std::string name_ = "POWER-Z";
    std::string volt_path_, curr_path_, temp_path_;
};

// ---------------------------------------------------------------------------
// ElmorLabs PMD2 — inline DC power meter on the PSU harness, read over USB CDC.
// Like PowerZProbe it is a SYSTEM instrument, never a card's rail: everything
// goes to sysvoltage_/syscurrent_/syspower_, which keeps board watts off the
// per-card graphs. (In mb-benchmark-gui that separation also keeps them out of
// power_for_device(), where they would inflate every efficiency figure; there
// is no such function here, but the families must not be merged either way.)
//
// Protocol lifted from mb-powermon.py's PMD2Probe (itself from
// ElmorLabs/PMD2-Python): 115200 8N1 raw; 0x01 -> 3 bytes {vid, pid, fw};
// 0x04 -> a 122-byte packed SensorStruct:
//
//   uint16 Vdd_mV; int16 Tchip; 10 x { int16 V_mV; int32 I_mA; int32 P_mW };
//   uint16 EpsPower, PciePower, MbPower, TotalPower;  (WHOLE WATTS)
//   uint8 Ocp[10];
//
// **Everything measurable is published and therefore always logged** — one
// TOTAL, three group aggregates, and all ten rails in V, A and W: 34 metrics.
// Selecting what to *graph* is a UI filter (Graphs -> PMD2), never a change to
// what is discovered: `discover()` runs once and the Logger header is written
// once at open, so a metric that came and went would break both the fixed
// header and the rule that two logs from one binary stay diffable.
//
// Resolution is why the rails matter even though the groups look sufficient:
// the four aggregates are uint16 WHOLE WATTS, so MB moves in 1 W steps — too
// coarse to show an accelerator's delta — while the rails are mV/mA/mW.
// Verified self-consistent 2026-09-18: EPS 93 + MB 26 = TOTAL 119, and
// ATX12V+5V+5VSB+3.3V = 25.4 W against MB's 26.
//
// Deliberately NOT published: Tchip (the meter's own STM32 die — not the system
// and not any card, the same reason PowerZ's in1..in5 are suppressed) and the
// OCP flag bytes. Rails reading zero (HPWR1, PCIE1..3 on an all-M.2 host) ARE
// published: a flat zero is a measurement, and suppressing them would make the
// CSV schema depend on what happened to be plugged in.
// ---------------------------------------------------------------------------
class PMD2Probe : public DeviceProbe {
public:
    const char* name() const override { return name_.c_str(); }

    bool discover() {
        for (const auto& t : glob_paths("/sys/class/tty/ttyACM*")) {
            const std::string dev = realpath_of(t + "/device/..");
            if (dev.empty()) continue;
            if (trim_sysfs(read_file(dev + "/idVendor")) != "0483") continue;
            if (trim_sysfs(read_file(dev + "/idProduct")) != "5740") continue;
            if (!open_port("/dev/" + basename_of(t))) continue;
            bdf_ = "usb " + basename_of(dev);  // matches the INA228 "usb <port>" form
            break;
        }
        if (fd_ < 0) return false;
        Sensor s{};
        if (!read_sensor(&s)) { close_port(); return false; }

        // Order is the display order: total, then groups, then rails. The
        // legend groups by contiguous runs of device index, so keeping them
        // together keeps PMD2 to one row.
        // Every family shares ONE CSV namespace: the column is <bdf>_<LABEL>
        // with the device prefix stripped, so a rail labelled just "ATX12V" in
        // three families would emit the SAME column three times. Suffix each
        // with its family, exactly as the INA228 metrics do
        // (<Card> INA228 POWER/VBUS/CURRENT) and for the same reason.
        syspower_metrics_.push_back({name_ + std::string(" TOTAL"), "W"});
        for (const char* g : kGroupNames)
            syspower_metrics_.push_back({name_ + " " + g + " POWER", "W"});
        for (const char* r : kRailNames)
            syspower_metrics_.push_back({name_ + " " + r + " POWER", "W"});
        syspower_values_.assign(syspower_metrics_.size(), kNaN);

        for (const char* r : kRailNames) {
            sysvoltage_metrics_.push_back({name_ + " " + r + " VBUS", "V"});
            syscurrent_metrics_.push_back({name_ + " " + r + " CURRENT", "A"});
        }
        sysvoltage_values_.assign(sysvoltage_metrics_.size(), kNaN);
        syscurrent_values_.assign(syscurrent_metrics_.size(), kNaN);

        note_ = "PMD2 fw v" + std::to_string(fw_);
        poll();
        return true;
    }

    void poll() override {
        Sensor s{};
        if (!read_sensor(&s)) {
            for (auto& v : syspower_values_) v = kNaN;
            for (auto& v : sysvoltage_values_) v = kNaN;
            for (auto& v : syscurrent_values_) v = kNaN;
            return;
        }
        size_t k = 0;
        syspower_values_[k++] = plausible_power(s.total_w);
        syspower_values_[k++] = plausible_power(s.eps_w);
        syspower_values_[k++] = plausible_power(s.pcie_w);
        syspower_values_[k++] = plausible_power(s.mb_w);
        for (int i = 0; i < 10; ++i)
            syspower_values_[k++] = plausible_power(s.rail_mw[i] / 1000.0);
        for (int i = 0; i < 10; ++i) {
            sysvoltage_values_[i] = s.rail_mv[i] / 1000.0;
            syscurrent_values_[i] = s.rail_ma[i] / 1000.0;
        }
    }

    ~PMD2Probe() override { close_port(); }

private:
    struct Sensor {
        double rail_mv[10], rail_ma[10], rail_mw[10];
        double eps_w, pcie_w, mb_w, total_w;
    };
    static const char* const kGroupNames[3];
    static const char* const kRailNames[10];
    static constexpr size_t kSensorSize = 122;

    bool open_port(const std::string& node) {
        fd_ = ::open(node.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) return false;
        termios t{};
        if (tcgetattr(fd_, &t) != 0) { close_port(); return false; }
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag |= (CLOCAL | CREAD);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;  // non-blocking; read_exact does its own timeout
        if (tcsetattr(fd_, TCSANOW, &t) != 0) { close_port(); return false; }
        tcflush(fd_, TCIOFLUSH);
        // Identify. A tty with the right USB ids that does not answer 0x01 is
        // not a PMD2, so this is the check, not the VID/PID match alone.
        unsigned char cmd = 0x01, buf[3];
        if (::write(fd_, &cmd, 1) != 1) { close_port(); return false; }
        if (read_exact(buf, sizeof buf, 1000) != sizeof buf) { close_port(); return false; }
        fw_ = buf[2];
        return true;
    }

    void close_port() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    // All n bytes or nothing. A short read would desynchronise every later
    // poll, so it is a failure and the buffer is flushed rather than patched.
    ssize_t read_exact(unsigned char* dst, size_t n, int timeout_ms) {
        size_t got = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (got < n && std::chrono::steady_clock::now() < deadline) {
            const ssize_t r = ::read(fd_, dst + got, n - got);
            if (r > 0) { got += static_cast<size_t>(r); continue; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return static_cast<ssize_t>(got);
    }

    bool read_sensor(Sensor* out) {
        if (fd_ < 0) return false;
        tcflush(fd_, TCIFLUSH);
        unsigned char cmd = 0x04;
        if (::write(fd_, &cmd, 1) != 1) return false;
        unsigned char b[kSensorSize];
        if (read_exact(b, kSensorSize, 500) != static_cast<ssize_t>(kSensorSize)) {
            tcflush(fd_, TCIFLUSH);
            return false;
        }
        auto u16 = [&](size_t o) { return static_cast<double>(b[o] | (b[o + 1] << 8)); };
        auto i16 = [&](size_t o) {
            return static_cast<double>(static_cast<int16_t>(b[o] | (b[o + 1] << 8)));
        };
        auto i32 = [&](size_t o) {
            return static_cast<double>(static_cast<int32_t>(
                static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
                (static_cast<uint32_t>(b[o + 2]) << 16) |
                (static_cast<uint32_t>(b[o + 3]) << 24)));
        };
        size_t o = 4;  // skip uint16 Vdd + int16 Tchip
        for (int i = 0; i < 10; ++i) {
            out->rail_mv[i] = i16(o); o += 2;
            out->rail_ma[i] = i32(o); o += 4;
            out->rail_mw[i] = i32(o); o += 4;
        }
        out->eps_w = u16(o);   o += 2;
        out->pcie_w = u16(o);  o += 2;
        out->mb_w = u16(o);    o += 2;
        out->total_w = u16(o);
        return true;
    }

    static std::string realpath_of(const std::string& p) {
        char buf[PATH_MAX];
        const char* r = realpath(p.c_str(), buf);
        return r ? std::string(r) : std::string();
    }

    std::string name_ = "PMD2";
    int fd_ = -1;
    int fw_ = -1;
};

const char* const PMD2Probe::kGroupNames[3] = {"EPS", "PCIE", "MB"};
const char* const PMD2Probe::kRailNames[10] = {
    "ATX12V", "ATX5V", "ATX5VSB", "ATX3.3V", "HPWR1",
    "EPS1", "EPS2", "PCIE1", "PCIE2", "PCIE3"};

#if MB_HAVE_FTDI
// ---------------------------------------------------------------------------
// INA228 external power meter over an FT232H USB->I2C bridge (libftdi1 MPSSE).
// Reference-grade rail power that the on-card telemetry can't give (e.g. Axelera
// M.2, Qualcomm IQ). One sensor per bridge is the norm; each bridge becomes its
// own probe/legend row. Bridges carry no USB serial here, so they're selected by
// libusb bus/address and named by enumeration order (`INA228#0`, `INA228#1`) —
// a config file will later pin each to a specific accelerator. The MPSSE-I2C +
// INA228 register/calibration path is lifted verbatim from the hardware-
// validated envic_ai_cpp `mb_power_smoke`; defaults mirror mb-powermon.py's
// INA228Probe (15 mΩ shunt, 5 A full-scale).
// ---------------------------------------------------------------------------
namespace ftdi {

constexpr uint16_t kVid = 0x0403, kPid = 0x6014;  // FT232H
// MPSSE clock-data opcodes not defined by ftdi.h.
constexpr uint8_t OP_WR_BYTES = 0x11, OP_WR_BITS = 0x13,
                  OP_RD_BYTES = 0x20, OP_RD_BITS = 0x22;
constexpr uint8_t PIN_SCL = 0x01, PIN_SDA = 0x02;
constexpr uint8_t DIR_DRIVE = PIN_SCL | PIN_SDA, DIR_READ = PIN_SCL;
constexpr int HOLD = 6;  // repeat a GPIO state N times for I2C setup/hold time

// MPSSE bit-banged I2C master on one specific FT232H (by libusb bus/address).
class Ft232hI2c {
public:
    Ft232hI2c(int usb_bus, int usb_addr) : bus_(usb_bus), addr_(usb_addr) {}
    ~Ft232hI2c() {
        if (ftdi_) {
            ftdi_set_bitmode(ftdi_, 0x00, BITMODE_RESET);
            ftdi_usb_close(ftdi_);
            ftdi_free(ftdi_);
        }
    }
    std::string error;

    bool open() {
        ftdi_ = ftdi_new();
        if (!ftdi_) { error = "ftdi_new failed"; return false; }
        if (ftdi_usb_open_bus_addr(ftdi_, (uint8_t)bus_, (uint8_t)addr_) < 0) {
            error = std::string("ftdi_usb_open: ") + ftdi_get_error_string(ftdi_);
            return false;
        }
        ftdi_usb_reset(ftdi_);
        ftdi_set_interface(ftdi_, INTERFACE_A);
        ftdi_set_latency_timer(ftdi_, 1);
        ftdi_tcioflush(ftdi_);
        if (ftdi_set_bitmode(ftdi_, 0x00, BITMODE_RESET) < 0) { error = "bitmode reset"; return false; }
        if (ftdi_set_bitmode(ftdi_, 0x00, BITMODE_MPSSE) < 0) { error = "bitmode MPSSE"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        buf_.clear();
        q(DIS_DIV_5); q(DIS_ADAPTIVE); q(EN_3_PHASE); q(LOOPBACK_END);
        q(TCK_DIVISOR); q(0xC7); q(0x00);  // ~100 kHz: 60MHz/(100kHz*3)-1 = 199
        gpio(PIN_SCL | PIN_SDA, DIR_DRIVE);
        if (!flush()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return true;
    }

    void start() {
        for (int i = 0; i < HOLD; ++i) gpio(PIN_SCL | PIN_SDA, DIR_DRIVE);
        for (int i = 0; i < HOLD; ++i) gpio(PIN_SCL,           DIR_DRIVE);
        for (int i = 0; i < HOLD; ++i) gpio(0x00,              DIR_DRIVE);
    }
    void stop() {
        for (int i = 0; i < HOLD; ++i) gpio(0x00,              DIR_DRIVE);
        for (int i = 0; i < HOLD; ++i) gpio(PIN_SCL,           DIR_DRIVE);
        for (int i = 0; i < HOLD; ++i) gpio(PIN_SCL | PIN_SDA, DIR_DRIVE);
        gpio(PIN_SCL | PIN_SDA, DIR_READ);
    }
    void rep_start() {
        for (int i = 0; i < HOLD; ++i) gpio(PIN_SDA, DIR_DRIVE);
        for (int i = 0; i < HOLD; ++i) gpio(PIN_SCL | PIN_SDA, DIR_DRIVE);
        start();
    }
    bool write_byte(uint8_t b) {
        gpio(0x00, DIR_DRIVE);
        q(OP_WR_BYTES); q(0x00); q(0x00); q(b);
        gpio(0x00, DIR_READ);
        q(OP_RD_BITS); q(0x00);
        q(SEND_IMMEDIATE);
        uint8_t ack = 0xFF;
        if (!flush_read(&ack, 1)) return false;
        return (ack & 0x01) == 0;
    }
    uint8_t read_byte(bool ack) {
        gpio(0x00, DIR_READ);
        q(OP_RD_BYTES); q(0x00); q(0x00);
        q(SEND_IMMEDIATE);
        uint8_t data = 0;
        flush_read(&data, 1);
        gpio(0x00, DIR_DRIVE);
        q(OP_WR_BITS); q(0x00); q(ack ? 0x00 : 0x80);
        flush();
        return data;
    }
    bool reg_read(uint8_t a, uint8_t reg, uint8_t* out, int n) {
        start();
        if (!write_byte((a << 1) | 0)) { stop(); error = "no ACK (addr+W)"; return false; }
        if (!write_byte(reg))          { stop(); error = "no ACK (reg)";    return false; }
        rep_start();
        if (!write_byte((a << 1) | 1)) { stop(); error = "no ACK (addr+R)"; return false; }
        for (int i = 0; i < n; ++i) out[i] = read_byte(i < n - 1);
        stop();
        return true;
    }
    bool ping(uint8_t a) {
        start();
        bool ack = write_byte((a << 1) | 0);
        stop();
        return ack;
    }

private:
    int bus_, addr_;
    ftdi_context* ftdi_ = nullptr;
    std::vector<uint8_t> buf_;
    void q(uint8_t b) { buf_.push_back(b); }
    void gpio(uint8_t val, uint8_t dir) { q(SET_BITS_LOW); q(val); q(dir); }
    bool flush() {
        if (buf_.empty()) return true;
        int rc = ftdi_write_data(ftdi_, buf_.data(), (int)buf_.size());
        bool ok = rc == (int)buf_.size();
        if (!ok) error = std::string("ftdi_write_data: ") + ftdi_get_error_string(ftdi_);
        buf_.clear();
        return ok;
    }
    bool flush_read(uint8_t* out, int n) {
        if (!flush()) return false;
        int got = 0;
        for (int tries = 0; tries < 1000 && got < n; ++tries) {
            int rc = ftdi_read_data(ftdi_, out + got, n - got);
            if (rc < 0) { error = "ftdi_read_data failed"; return false; }
            got += rc;
            if (rc == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (got < n) { error = "short read from FT232H"; return false; }
        return true;
    }
};

// TI INA228 register driver (big-endian). POWER is full 24-bit (no >>4).
struct Ina228 {
    Ft232hI2c* bus = nullptr;
    uint8_t addr = 0x40;
    double current_lsb = 0.0;
    enum Reg : uint8_t {
        CONFIG = 0x00, ADC_CONFIG = 0x01, SHUNT_CAL = 0x02,
        VBUS = 0x05, DIETEMP = 0x06, CURRENT = 0x07, POWER = 0x08,
        ENERGY = 0x09, CHARGE = 0x0A, DIAG_ALRT = 0x0B, BUVL = 0x0F,
        DEVICE_ID = 0x3F,
    };
    // DIAG_ALRT bit 15 latches a trip until the register is read; bit 3 is the
    // bus under-limit flag. Verified on hardware 2026-09-07: with BUVL set above
    // the live rail the flag sets, below it clears, and a read clears the latch.
    // That read-to-clear is the point — polled once a second it reports whether
    // the rail dipped AT ANY POINT in that second, catching transients far
    // shorter than we could sample directly. Note ADC_CONFIG averages 64
    // conversions (~67 ms), and the comparator sees that average, so a dip much
    // shorter than that is still smoothed away before it can be detected.
    static constexpr uint16_t kAlatch = 0x8000;
    static constexpr uint16_t kBusUnderLimit = 1u << 3;
    // BUVL is unsigned, 3.125 mV/LSB — 16x the VBUS LSB, i.e. it compares
    // against the top 16 bits of the 20-bit VBUS result. Measured, not assumed.
    static constexpr double kBuvlLsbV = 3.125e-3;
    // CONFIG bit 14. Writing it clears ENERGY and CHARGE; it is an ordinary R/W
    // bit, so it has to be written back to 0 or the accumulators stay pinned.
    static constexpr uint16_t kRstAcc = 0x4000;
    bool write_reg16(uint8_t reg, uint16_t v) {
        bus->start();
        bool ok = bus->write_byte((addr << 1) | 0) && bus->write_byte(reg) &&
                  bus->write_byte((uint8_t)(v >> 8)) && bus->write_byte((uint8_t)(v & 0xFF));
        bus->stop();
        return ok;
    }
    uint16_t read16(uint8_t reg, bool* ok) {
        uint8_t b[2] = {0, 0};
        *ok = bus->reg_read(addr, reg, b, 2);
        return (uint16_t)((b[0] << 8) | b[1]);
    }
    uint32_t read24(uint8_t reg, bool* ok) {
        uint8_t b[3] = {0, 0, 0};
        *ok = bus->reg_read(addr, reg, b, 3);
        return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    }
    uint32_t read24_raw20(uint8_t reg, bool* ok) { return read24(reg, ok) >> 4; }
    // ENERGY and CHARGE are 40-bit. reg_read() already takes an arbitrary byte
    // count, so this needs no new bus plumbing.
    uint64_t read40(uint8_t reg, bool* ok) {
        uint8_t b[5] = {0, 0, 0, 0, 0};
        *ok = bus->reg_read(addr, reg, b, 5);
        uint64_t v = 0;
        for (int i = 0; i < 5; ++i) v = (v << 8) | b[i];
        return v;
    }
    bool configure(double shunt_res, double max_current) {
        current_lsb = max_current / 524288.0;  // 2^19
        uint16_t cal = (uint16_t)(13107200000.0 * current_lsb * shunt_res + 0.5);
        // MODE 0xF is continuous bus + shunt + *temperature*, so DIETEMP is
        // already being converted — reading it needs no config change.
        uint16_t adc = (0xF << 12) | (5 << 9) | (5 << 6) | (5 << 3) | 3;  // 0xFB6B
        // Pulse RSTACC so the session's energy/charge series start at zero.
        // This is a device-state write, like the Axelera collector level: a
        // second client reading these accumulators would see them cleared. Kept
        // because nothing else on either host touches them, and a series that
        // began at an arbitrary carried-over value would be unreadable.
        if (!(write_reg16(CONFIG, kRstAcc) && write_reg16(CONFIG, 0x0000) &&
              write_reg16(ADC_CONFIG, adc) && write_reg16(SHUNT_CAL, cal)))
            return false;
        // Arm the latched bus-undervoltage detector. The threshold is the M.2
        // 3.3 V rail's own lower limit (3.3 V -9% = 3.003 V), so a trip means
        // the supply left spec, not merely that it sagged. Like RSTACC this is
        // device state another client would see; it drives only the ALERT pin
        // (unwired here) and the status flag, never the conversions.
        undervolt_limit_v = 3.0;
        return write_reg16(BUVL, (uint16_t)(undervolt_limit_v / kBuvlLsbV + 0.5)) &&
               write_reg16(DIAG_ALRT, kAlatch);
    }
    double power(bool* ok) { return read24(POWER, ok) * 3.2 * current_lsb; }  // W
    // Bus voltage. 24-bit register, 20-bit result in bits 23:4, 195.3125 uV/LSB.
    double bus_voltage(bool* ok) { return read24_raw20(VBUS, ok) * 195.3125e-6; }
    // Current. Same 20-in-24 layout as VBUS but SIGNED, so read24_raw20() is the
    // wrong helper on its own — unsigned it would report ~1.05 MA instead of
    // -3.9 A. Sign-extend from bit 19, the same way charge() does from bit 39.
    //
    // Reads NEGATIVE on every rail here, and that is the harness, not a fault:
    // IN+/IN- are wired the other way round, which is also why CHARGE is
    // negative while POWER and ENERGY (unsigned registers) are not. Reported as
    // measured rather than abs()'d — the sign is a real fact about the wiring,
    // and hiding it would mask a later rewire.
    double current(bool* ok) {
        uint32_t raw = read24_raw20(CURRENT, ok);
        if (raw & (1u << 19)) raw |= ~((1u << 20) - 1);
        return (double)(int32_t)raw * current_lsb;  // A
    }
    // True if the rail went below BUVL at any point since the last call. Reading
    // DIAG_ALRT clears the latch, so this is edge-triggered per poll.
    bool undervoltage(bool* ok) {
        return (read16(DIAG_ALRT, ok) & kBusUnderLimit) != 0;
    }
    double undervolt_limit_v = 0.0;
    // Monitor die temperature: signed 16-bit, 7.8125 m°C/LSB. A *full* 16-bit
    // register, so read24_raw20() is the wrong helper — and read16() is
    // unsigned, hence the cast.
    double die_temp(bool* ok) {
        return (int16_t)read16(DIETEMP, ok) * 0.0078125;  // °C
    }
    // 40-bit unsigned. LSB = 16 * 3.2 * current_lsb (488 uJ as configured), so
    // it rolls over after ~3.4 years at 5 W — not a practical concern.
    double energy(bool* ok) {
        return (double)read40(ENERGY, ok) * 16.0 * 3.2 * current_lsb;  // J
    }
    // 40-bit *signed*: sign-extend from bit 39.
    //
    // On this rig all four shunts read NEGATIVE charge, and that is the harness,
    // not a decode bug: measured 2026-08-08, P / |dQ/dt| comes out at 3.16-3.20 V
    // on every rail — the M.2 3.3 V rail, consistent to 1%. So the magnitude is
    // right and only the direction is flipped, because IN+/IN- are wired the
    // other way round. POWER and ENERGY are unsigned registers, so they are
    // unaffected and stay positive.
    //
    // Reported as measured rather than abs()'d: the sign is a real fact about
    // the wiring, and silently discarding it would hide a rewire later. Divide
    // by elapsed seconds for average current, and mind the sign.
    double charge(bool* ok) {
        uint64_t raw = read40(CHARGE, ok);
        if (raw & (1ULL << 39)) raw |= ~((1ULL << 40) - 1);
        return (double)(int64_t)raw * current_lsb;  // C
    }
};

// Enumerate FT232H bridges via sysfs. `bus`/`addr` (busnum/devnum) open the
// device with libftdi; `port` is the sysfs kernel name (e.g. "1-1"), the stable
// physical-port path used as the config key — unlike devnum, it survives replug.
struct Loc { int bus, addr; std::string port; };
inline std::vector<Loc> enumerate_bridges() {
    std::vector<Loc> out;
    for (const auto& d : glob_paths("/sys/bus/usb/devices/*")) {
        if (std::strtoul(trim_sysfs(read_file(d + "/idVendor")).c_str(), nullptr, 16) != kVid) continue;
        if (std::strtoul(trim_sysfs(read_file(d + "/idProduct")).c_str(), nullptr, 16) != kPid) continue;
        int bus = std::atoi(trim_sysfs(read_file(d + "/busnum")).c_str());
        int addr = std::atoi(trim_sysfs(read_file(d + "/devnum")).c_str());
        if (bus && addr) out.push_back({bus, addr, basename_of(d)});
    }
    return out;
}

// Optional user map: USB port-path → legend label (which accelerator's rail this
// INA228 measures). File: $MB_INA228_CONFIG, else
// $XDG_CONFIG_HOME/mb-powermon-gui/ina228.conf, else ~/.config/…. Lines are
// "<port> = <label>[, <flag>...]", '#' starts a comment. Missing file → empty
// map (probes fall back to "INA228#<n>").
//
// The one flag today is `invert`, which negates CURRENT and CHARGE for that
// rail. It exists because IN+/IN- can be wired either way round on the shunt
// breakout, and the INA228 reports the shunt drop signed: reversed leads give a
// negative current for a card that is drawing. VBUS, POWER and ENERGY are
// unsigned registers and are unaffected, which is why only two families move.
//
// Deliberately per-rail, not global: the harness can be corrected one breakout
// at a time, and a global switch would then be wrong for every other rail.
struct RailSpec {
    std::string label;
    bool invert = false;
};
inline std::map<std::string, RailSpec> load_label_map() {
    std::string path;
    if (const char* e = std::getenv("MB_INA228_CONFIG")) {
        path = e;
    } else {
        std::string base;
        if (const char* x = std::getenv("XDG_CONFIG_HOME")) base = x;
        else if (const char* h = std::getenv("HOME")) base = std::string(h) + "/.config";
        if (!base.empty()) path = base + "/mb-powermon-gui/ina228.conf";
    }
    std::map<std::string, RailSpec> m;
    if (path.empty()) return m;
    std::ifstream f(path);
    if (!f) return m;
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    std::string line;
    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;
        RailSpec spec;
        // "<label>[, flag[, flag...]]" — unknown flags are ignored rather than
        // fatal, so an older binary reading a newer config still maps the rail.
        std::size_t start = 0;
        for (int field = 0;; ++field) {
            const std::size_t comma = val.find(',', start);
            std::string tok = trim(val.substr(
                start, comma == std::string::npos ? comma : comma - start));
            std::string low = tok;
            for (auto& c : low) c = (char)std::tolower((unsigned char)c);
            if (field == 0) spec.label = tok;
            else if (low == "invert") spec.invert = true;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (!spec.label.empty()) m[key] = spec;
    }
    return m;
}
}  // namespace ftdi

// One FT232H bridge and the INA228(s) on it, as a DeviceProbe. Passive w.r.t.
// the accelerator — it measures the rail, never touches the NPU.
class INA228Probe : public DeviceProbe {
public:
    // `label` (from the config file, keyed on `port`) names the accelerator whose
    // rail this INA228 measures; the legend row reads "INA228 - <label>" so the
    // sensor is always identifiable. Unmapped bridges fall back to "INA228#<n>".
    INA228Probe(int usb_bus, int usb_addr, std::string port, std::string label,
                int index, bool invert = false)
        : usb_bus_(usb_bus), usb_addr_(usb_addr), port_(std::move(port)),
          invert_(invert) {
        name_ = label.empty() ? "INA228#" + std::to_string(index)
                              : "INA228 - " + label;
        color_alias_ = label;  // share the mapped accelerator's swatch, if present
    }
    const char* name() const override { return name_.c_str(); }

    bool discover() {
        bus_ = std::make_unique<ftdi::Ft232hI2c>(usb_bus_, usb_addr_);
        if (!bus_->open()) {
            note_ = "FT232H open failed: " + bus_->error;
            return false;
        }
        // Canonical Adafruit-strap addresses; usually just one responds.
        for (uint8_t a : {0x40, 0x41, 0x44, 0x45}) {
            if (!bus_->ping(a)) continue;
            ftdi::Ina228 s;
            s.bus = bus_.get();
            s.addr = a;
            if (!s.configure(0.015, 5.0)) continue;
            sensors_.push_back(s);
            // All four families are declared here unconditionally, even though
            // the first read has not happened yet. The Logger writes its header
            // once at open, so a metric that only appeared after a successful
            // read would lose its column for the whole session.
            power_metrics_.push_back({name_ + " POW", "W"});
            power_values_.push_back(kNaN);
            temp_metrics_.push_back({name_ + " TEMP", "°C"});
            temp_values_.push_back(kNaN);
            energy_metrics_.push_back({name_ + " ENERGY", "J"});
            energy_values_.push_back(kNaN);
            charge_metrics_.push_back({name_ + " CHARGE", "C"});
            charge_values_.push_back(kNaN);
            voltage_metrics_.push_back({name_ + " VBUS", "V"});
            voltage_values_.push_back(kNaN);
            current_metrics_.push_back({name_ + " CURRENT", "A"});
            current_values_.push_back(kNaN);
        }
        if (sensors_.empty()) {
            note_ = "FT232H present but no INA228 responded";
            return false;
        }
        // If a bridge carries more than one sensor, disambiguate by I2C address.
        // Every family is indexed by the same sensor index, so all six have to
        // be relabelled together or they desynchronise.
        if (sensors_.size() > 1)
            for (size_t i = 0; i < sensors_.size(); ++i) {
                char l[40];
                auto tag = [&](const char* suffix) {
                    std::snprintf(l, sizeof l, "%s 0x%02X %s", name_.c_str(),
                                  sensors_[i].addr, suffix);
                    return std::string(l);
                };
                power_metrics_[i].label = tag("POW");
                temp_metrics_[i].label = tag("TEMP");
                energy_metrics_[i].label = tag("ENERGY");
                charge_metrics_[i].label = tag("CHARGE");
                voltage_metrics_[i].label = tag("VBUS");
                current_metrics_[i].label = tag("CURRENT");
            }
        bdf_ = "usb " + port_;  // stable physical-port locator (e.g. "usb 1-1")
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // settle
        return true;
    }

    void poll() override {
        for (size_t i = 0; i < sensors_.size(); ++i) {
            bool ok = false;
            double p = sensors_[i].power(&ok);
            power_values_[i] = ok ? plausible_power(p) : kNaN;
            // NaN only on an I2C failure — never 0. Zero is a real reading here
            // (0.0 W is the documented INA228 overflow signal, and a freshly
            // reset accumulator genuinely reads 0 J), so the two must not be
            // conflated. Same rule the Logger relies on.
            ok = false;
            const double t = sensors_[i].die_temp(&ok);
            temp_values_[i] = ok ? plausible_temp(t) : kNaN;
            ok = false;
            const double j = sensors_[i].energy(&ok);
            energy_values_[i] = ok ? j : kNaN;
            ok = false;
            const double c = sensors_[i].charge(&ok);
            // `invert` from ina228.conf, applied to the two SIGNED families only.
            // VBUS/POWER/ENERGY come from unsigned registers and cannot carry a
            // polarity error, so flipping them would be wrong, not merely
            // unnecessary.
            charge_values_[i] = ok ? (invert_ ? -c : c) : kNaN;
            ok = false;
            const double v = sensors_[i].bus_voltage(&ok);
            voltage_values_[i] = ok ? v : kNaN;
            ok = false;
            const double a = sensors_[i].current(&ok);
            current_values_[i] = ok ? (invert_ ? -a : a) : kNaN;
            // Latched bus-undervoltage. Reading DIAG_ALRT clears the latch, so
            // this answers "did the rail leave spec at any point in the last
            // second?" rather than "is it out of spec right now" — the only way
            // a 1 Hz poll can see a transient brown-out. Reported through the
            // note sink, which logs on transition rather than every second.
            ok = false;
            const bool uv = sensors_[i].undervoltage(&ok);
            if (ok && uv) ++undervolt_count_;
            if (undervolt_count_ != last_undervolt_count_) {
                char m[160];
                std::snprintf(m, sizeof m,
                              "%s: bus voltage dipped below %.2f V (%d time%s "
                              "since discovery) — supply may be browning out",
                              name_.c_str(), sensors_[i].undervolt_limit_v,
                              undervolt_count_, undervolt_count_ == 1 ? "" : "s");
                note_ = m;
                last_undervolt_count_ = undervolt_count_;
            }
        }
    }

private:
    int usb_bus_, usb_addr_;
    std::string port_, name_;
    std::unique_ptr<ftdi::Ft232hI2c> bus_;
    std::vector<ftdi::Ina228> sensors_;
    bool invert_ = false;   // ina228.conf `invert` — reversed IN+/IN- on this rail
    int undervolt_count_ = 0, last_undervolt_count_ = 0;
};
#endif  // MB_HAVE_FTDI

}  // namespace

void Probes::discover(std::vector<std::string>* notes) {
    auto try_add = [&](std::unique_ptr<DeviceProbe> p, bool ok) {
        if (ok) {
            if (notes) {
                // A present device with a note (e.g. version mismatch) is silent
                // for a nameable reason — report that instead of a sensor count.
                std::string msg = std::string(p->name()) + ": ";
                if (!p->note().empty()) {
                    msg += p->note();
                } else {
                    const size_t nt = p->temp_metrics().size();
                    const size_t np = p->power_metrics().size();
                    // The system families count too, or an inline supply meter
                    // reports as "1 temp sensor(s)" and looks half-detected.
                    const size_t ns = p->sysvoltage_metrics().size() +
                                      p->syscurrent_metrics().size() +
                                      p->syspower_metrics().size();
                    std::vector<std::string> parts;
                    if (nt) parts.push_back(std::to_string(nt) + " temp");
                    if (np) parts.push_back(std::to_string(np) + " power");
                    if (ns) parts.push_back(std::to_string(ns) + " system");
                    if (parts.empty()) parts.push_back("0");
                    for (size_t i = 0; i < parts.size(); ++i)
                        msg += (i ? " + " : "") + parts[i];
                    msg += " sensor(s)";
                }
                notes->push_back(std::move(msg));
            }
            devices_.push_back(std::move(p));
        } else if (notes) {
            notes->push_back(std::string(p->name()) + ": " +
                             (p->note().empty() ? "not present / no data"
                                                : p->note()));
        }
    };

#if MB_HAVE_HAILO
    {
        auto p = std::make_unique<HailoProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
#endif
    {
        auto p = std::make_unique<MemryXProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
    {
        auto p = std::make_unique<DeepXProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
    {
        auto p = std::make_unique<AxeleraProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
    {
        auto p = std::make_unique<QualcommIQProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
    {
        auto p = std::make_unique<BoardThermalProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
    {
        // Before the INA228 bridges: both are external USB meters, and this
        // keeps the board-level rows ahead of the per-rail ones.
        auto p = std::make_unique<PowerZProbe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
    {
        // Same slot in the order as PowerZ, for the same reason: a board-level
        // meter reads before the per-rail shunts. Not exclusive with it — a
        // host can carry both, measuring the same board at different points
        // (PMD2 on the PSU harness, POWER-Z on a USB-C input).
        auto p = std::make_unique<PMD2Probe>();
        bool ok = p->discover();
        try_add(std::move(p), ok);
    }
#if MB_HAVE_FTDI
    // One probe per FT232H bridge (each carries an INA228). Bridges have no
    // serials, so the config file maps each by its stable USB port-path to an
    // accelerator label; unmapped bridges fall back to "INA228#<n>".
    {
        auto labels = ftdi::load_label_map();
        int idx = 0;
        for (const auto& br : ftdi::enumerate_bridges()) {
            auto it = labels.find(br.port);
            const std::string label =
                (it != labels.end()) ? it->second.label : std::string();
            const bool invert = (it != labels.end()) && it->second.invert;
            auto p = std::make_unique<INA228Probe>(br.bus, br.addr, br.port,
                                                   label, idx++, invert);
            bool ok = p->discover();
            try_add(std::move(p), ok);
        }
    }
#endif

    flatten();
}

// Emission order that keeps an aliased device (a mapped INA228) adjacent to the
// device it names. This is not cosmetic: a folded metric takes its target's
// device index, and the legend groups by *contiguous runs* of that index — so a
// folded reading emitted out of position would open a second legend row for a
// card that already has one.
//
// The INA228 goes *before* its target, so a card's legend row reads
// "INA228 · TS0 · TS1". Every folded family uses this one order, which is what
// puts the INA228 cell in the same legend column on the Power, Temperature and
// Accumulated Energy graphs — they are read side by side, so they have to line
// up. Everything unaliased keeps discovery order.
std::vector<size_t> Probes::alias_device_order() const {
    std::vector<size_t> order;
    std::vector<bool> done(devices_.size(), false);
    for (size_t k = 0; k < devices_.size(); ++k) {
        if (!devices_[k]->color_alias().empty()) continue;  // placed via its target
        for (size_t a = 0; a < devices_.size(); ++a)  // INA228s aliased to k, first
            if (!done[a] && devices_[a]->color_alias() == devices_[k]->name()) {
                order.push_back(a);
                done[a] = true;
            }
        order.push_back(k);
        done[k] = true;
    }
    for (size_t a = 0; a < devices_.size(); ++a)  // any alias that matched nothing
        if (!done[a]) order.push_back(a);
    return order;
}

// If devices_[k] is a mapped INA228 (color_alias set) whose named accelerator is
// present *and* a PCIe (M.2) device, return that accelerator's index — the INA228
// reading should fold onto its row so the row's max spans both the on-die and
// shunt readings. Otherwise -1 (the INA228 keeps its own standalone row).
int Probes::pcie_merge_target(size_t k) const {
    const std::string& alias = devices_[k]->color_alias();
    if (alias.empty()) return -1;
    static const std::regex pcie(
        R"(^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-9a-fA-F]$)");
    for (size_t j = 0; j < devices_.size(); ++j)
        if (j != k && devices_[j]->name() == alias &&
            std::regex_match(devices_[j]->bdf(), pcie))
            return static_cast<int>(j);
    return -1;
}

void Probes::flatten() {
    temp_metrics_.clear();
    power_metrics_.clear();
    freq_metrics_.clear();
    energy_metrics_.clear();
    charge_metrics_.clear();
    voltage_metrics_.clear();
    current_metrics_.clear();
    sysvoltage_metrics_.clear();
    syscurrent_metrics_.clear();
    syspower_metrics_.clear();
    auto stamp = [](MetricInfo& m, size_t k, DeviceProbe* d) {
        m.device = static_cast<int>(k);
        m.device_name = d->name();
        m.bdf = d->bdf();
        m.color_alias = d->color_alias();
    };

    // A PCIe-mapped INA228 folds onto the accelerator it names: the metric takes
    // that accelerator's device / name / bdf, so it shares the legend row, the
    // colour and the per-device aggregate.
    //
    // `suffix` is what distinguishes the four families once folded — POWER /
    // TEMP / ENERGY / CHARGE. They would otherwise all be "<accel> INA228".
    auto emit = [&](const std::vector<MetricInfo>& (DeviceProbe::*get)() const,
                    const char* suffix, std::vector<MetricInfo>& out) {
        for (size_t k : dev_order_) {
            const int tgt = pcie_merge_target(k);
            for (auto m : (devices_[k].get()->*get)()) {
                if (tgt >= 0) {
                    m.device = tgt;
                    m.device_name = devices_[tgt]->name();
                    m.bdf = devices_[tgt]->bdf();
                    m.color_alias.clear();
                    m.label = std::string(devices_[tgt]->name()) + " INA228" + suffix;
                } else {
                    stamp(m, k, devices_[k].get());
                }
                out.push_back(std::move(m));
            }
        }
    };

    dev_order_ = alias_device_order();
    emit(&DeviceProbe::temp_metrics, " TEMP", temp_metrics_);
    emit(&DeviceProbe::power_metrics, " POWER", power_metrics_);
    emit(&DeviceProbe::energy_metrics, " ENERGY", energy_metrics_);
    emit(&DeviceProbe::charge_metrics, " CHARGE", charge_metrics_);
    emit(&DeviceProbe::voltage_metrics, " VBUS", voltage_metrics_);
    emit(&DeviceProbe::current_metrics, " CURRENT", current_metrics_);
    emit(&DeviceProbe::sysvoltage_metrics, " SYS VBUS", sysvoltage_metrics_);
    emit(&DeviceProbe::syscurrent_metrics, " SYS CURRENT", syscurrent_metrics_);
    emit(&DeviceProbe::syspower_metrics, " SYS POWER", syspower_metrics_);

    // Frequency follows plain discovery order — there is no INA228-style
    // folding to do, since a clock always belongs to the card reporting it.
    // That is also why it is not routed through emit(), which applies the
    // alias ordering the folded families need.
    for (size_t k = 0; k < devices_.size(); ++k)
        for (auto m : devices_[k]->freq_metrics()) {
            stamp(m, k, devices_[k].get());
            freq_metrics_.push_back(std::move(m));
        }

    temp_values_.assign(temp_metrics_.size(), kNaN);
    power_values_.assign(power_metrics_.size(), kNaN);
    freq_values_.assign(freq_metrics_.size(), kNaN);
    energy_values_.assign(energy_metrics_.size(), kNaN);
    charge_values_.assign(charge_metrics_.size(), kNaN);
    voltage_values_.assign(voltage_metrics_.size(), kNaN);
    current_values_.assign(current_metrics_.size(), kNaN);
    sysvoltage_values_.assign(sysvoltage_metrics_.size(), kNaN);
    syscurrent_values_.assign(syscurrent_metrics_.size(), kNaN);
    syspower_values_.assign(syspower_metrics_.size(), kNaN);
}

void Probes::poll() {
    for (auto& d : devices_) d->poll();
    // Each value vector is aligned to its own reordered metric list, so the fill
    // has to walk the same order flatten() emitted in — not discovery order.
    auto fill = [&](const std::vector<double>& (DeviceProbe::*get)() const,
                    std::vector<double>& out) {
        size_t i = 0;
        for (size_t k : dev_order_)
            for (double v : (devices_[k].get()->*get)())
                if (i < out.size()) out[i++] = v;
    };
    fill(&DeviceProbe::temp_values, temp_values_);
    fill(&DeviceProbe::power_values, power_values_);
    fill(&DeviceProbe::energy_values, energy_values_);
    fill(&DeviceProbe::charge_values, charge_values_);
    fill(&DeviceProbe::voltage_values, voltage_values_);
    fill(&DeviceProbe::current_values, current_values_);
    fill(&DeviceProbe::sysvoltage_values, sysvoltage_values_);
    fill(&DeviceProbe::syscurrent_values, syscurrent_values_);
    fill(&DeviceProbe::syspower_values, syspower_values_);

    // Discovery order, matching flatten() above — not dev_order_.
    size_t fi = 0;
    for (auto& d : devices_)
        for (double v : d->freq_values()) freq_values_[fi++] = v;
}
