#include "Probes.h"

#include <fcntl.h>
#include <glob.h>
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
                temp_values_[i] =
                    raw.empty() ? kNaN : plausible_temp(std::stod(raw) / 1000.0);
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

        poll();  // seed values + set note_ (version mismatch / idle / no tool)
        return true;
    }

    void poll() override {
        auto temps = read_temps();
        for (size_t i = 0; i < temp_values_.size(); ++i)
            temp_values_[i] = (i < temps.size()) ? temps[i] : kNaN;
    }

private:
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
        VBUS = 0x05, CURRENT = 0x07, POWER = 0x08, DEVICE_ID = 0x3F,
    };
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
    bool configure(double shunt_res, double max_current) {
        current_lsb = max_current / 524288.0;  // 2^19
        uint16_t cal = (uint16_t)(13107200000.0 * current_lsb * shunt_res + 0.5);
        uint16_t adc = (0xF << 12) | (5 << 9) | (5 << 6) | (5 << 3) | 3;  // 0xFB6B
        return write_reg16(CONFIG, 0x0000) && write_reg16(ADC_CONFIG, adc) &&
               write_reg16(SHUNT_CAL, cal);
    }
    double power(bool* ok) { return read24(POWER, ok) * 3.2 * current_lsb; }  // W
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
// "<port> = <label>", '#' starts a comment. Missing file → empty map (probes
// fall back to "INA228#<n>").
inline std::map<std::string, std::string> load_label_map() {
    std::string path;
    if (const char* e = std::getenv("MB_INA228_CONFIG")) {
        path = e;
    } else {
        std::string base;
        if (const char* x = std::getenv("XDG_CONFIG_HOME")) base = x;
        else if (const char* h = std::getenv("HOME")) base = std::string(h) + "/.config";
        if (!base.empty()) path = base + "/mb-powermon-gui/ina228.conf";
    }
    std::map<std::string, std::string> m;
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
        if (!key.empty() && !val.empty()) m[key] = val;
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
                int index)
        : usb_bus_(usb_bus), usb_addr_(usb_addr), port_(std::move(port)) {
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
            power_metrics_.push_back({name_ + " POW", "W"});
            power_values_.push_back(kNaN);
        }
        if (sensors_.empty()) {
            note_ = "FT232H present but no INA228 responded";
            return false;
        }
        // If a bridge carries more than one sensor, disambiguate by I2C address.
        if (sensors_.size() > 1)
            for (size_t i = 0; i < sensors_.size(); ++i) {
                char l[24];
                std::snprintf(l, sizeof l, "%s 0x%02X", name_.c_str(), sensors_[i].addr);
                power_metrics_[i].label = l;
            }
        bdf_ = "usb " + port_;  // stable physical-port locator (e.g. "usb 1-1")
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // settle
        return true;
    }

    void poll() override {
        for (size_t i = 0; i < sensors_.size(); ++i) {
            bool ok = false;
            double p = sensors_[i].power(&ok);
            power_values_[i] = ok ? p : kNaN;
        }
    }

private:
    int usb_bus_, usb_addr_;
    std::string port_, name_;
    std::unique_ptr<ftdi::Ft232hI2c> bus_;
    std::vector<ftdi::Ina228> sensors_;
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
                    std::vector<std::string> parts;
                    if (nt) parts.push_back(std::to_string(nt) + " temp");
                    if (np) parts.push_back(std::to_string(np) + " power");
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
#if MB_HAVE_FTDI
    // One probe per FT232H bridge (each carries an INA228). Bridges have no
    // serials, so the config file maps each by its stable USB port-path to an
    // accelerator label; unmapped bridges fall back to "INA228#<n>".
    {
        auto labels = ftdi::load_label_map();
        int idx = 0;
        for (const auto& br : ftdi::enumerate_bridges()) {
            auto it = labels.find(br.port);
            std::string label = (it != labels.end()) ? it->second : std::string();
            auto p = std::make_unique<INA228Probe>(br.bus, br.addr, br.port,
                                                   label, idx++);
            bool ok = p->discover();
            try_add(std::move(p), ok);
        }
    }
#endif

    flatten();
}

// Device order for the *power* section: an aliased device (a mapped INA228) is
// grouped just before the device it names, and the INA228 comes first. So
// "INA228 - Hailo" sits immediately above "Hailo". Everything else keeps
// discovery order. (Temperature stays plain discovery order — INA228 has none.)
std::vector<size_t> Probes::power_device_order() const {
    std::vector<size_t> order;
    std::vector<bool> done(devices_.size(), false);
    for (size_t k = 0; k < devices_.size(); ++k) {
        if (!devices_[k]->color_alias().empty()) continue;  // placed via its target
        for (size_t a = 0; a < devices_.size(); ++a)        // INA228s aliased to k, first
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
    auto stamp = [](MetricInfo& m, size_t k, DeviceProbe* d) {
        m.device = static_cast<int>(k);
        m.device_name = d->name();
        m.bdf = d->bdf();
        m.color_alias = d->color_alias();
    };
    for (size_t k = 0; k < devices_.size(); ++k)
        for (auto m : devices_[k]->temp_metrics()) {
            stamp(m, k, devices_[k].get());
            temp_metrics_.push_back(std::move(m));
        }
    // Power in grouped order (power_device_order places a mapped INA228 right
    // before its accelerator). A PCIe-mapped INA228 folds onto that accelerator:
    // its metric takes the accelerator's device / name / bdf (so it shares the
    // row, color, and per-device max) and is labelled "<accel> INA228".
    power_dev_order_ = power_device_order();
    for (size_t k : power_dev_order_) {
        int tgt = pcie_merge_target(k);
        for (auto m : devices_[k]->power_metrics()) {
            if (tgt >= 0) {
                m.device = tgt;
                m.device_name = devices_[tgt]->name();
                m.bdf = devices_[tgt]->bdf();
                m.color_alias.clear();
                m.label = std::string(devices_[tgt]->name()) + " INA228";
            } else {
                stamp(m, k, devices_[k].get());
            }
            power_metrics_.push_back(std::move(m));
        }
    }
    temp_values_.assign(temp_metrics_.size(), kNaN);
    power_values_.assign(power_metrics_.size(), kNaN);
}

void Probes::poll() {
    for (auto& d : devices_) d->poll();
    size_t ti = 0;
    for (auto& d : devices_)
        for (double v : d->temp_values()) temp_values_[ti++] = v;
    size_t pi = 0;  // power_values_ is aligned to power_metrics_ (reordered)
    for (size_t k : power_dev_order_)
        for (double v : devices_[k]->power_values()) power_values_[pi++] = v;
}
