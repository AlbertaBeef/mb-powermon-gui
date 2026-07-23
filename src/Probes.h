// Passive telemetry collection for the edge-AI NPUs on this host.
//
// "Passive" = never claims a device or perturbs another application's use of
// it: DeepX via `dxrt-cli -s` (reads the kernel driver), MemryX via sysfs/hwmon,
// Axelera via `triton_trace --peek` (reads the collector log, no Context claim).
// No GTK dependency — pure data, mirroring the Python mb-powermon probes.
#pragma once

#include <memory>
#include <string>
#include <vector>

// One trace / legend row. `label` is device-qualified, e.g. "DeepX T0".
// `device` is the owning device's global index (assigned at discovery), so the
// UI can color every metric from the same device identically.
struct MetricInfo {
    std::string label;
    std::string unit;         // "°C" or "W"
    int device = -1;          // filled during discovery
    std::string device_name;  // "Hailo", "DeepX", ...
    std::string bdf;          // PCIe BDF, e.g. "0000:01:00.0"
};

// A single device's telemetry source.
class DeviceProbe {
public:
    virtual ~DeviceProbe() = default;
    virtual const char* name() const = 0;
    const std::string& bdf() const { return bdf_; }

    // Refresh readings. Fills temp_values()/power_values(), aligned to the
    // corresponding *_metrics() lists; a missing reading is NaN.
    virtual void poll() = 0;

    const std::vector<MetricInfo>& temp_metrics() const { return temp_metrics_; }
    const std::vector<double>& temp_values() const { return temp_values_; }
    const std::vector<MetricInfo>& power_metrics() const { return power_metrics_; }
    const std::vector<double>& power_values() const { return power_values_; }

protected:
    std::vector<MetricInfo> temp_metrics_;
    std::vector<double> temp_values_;
    std::vector<MetricInfo> power_metrics_;
    std::vector<double> power_values_;
    std::string bdf_;
};

// Discovers every supported device and presents their metrics as two flat,
// stable lists (temperature and power). The lists are fixed after discover();
// poll() only refreshes the aligned value vectors.
class Probes {
public:
    // Detect devices and build the metric lists. `notes` (optional) collects
    // human-readable diagnostics about what was / wasn't found.
    void discover(std::vector<std::string>* notes = nullptr);
    void poll();

    const std::vector<MetricInfo>& temp_metrics() const { return temp_metrics_; }
    const std::vector<double>& temp_values() const { return temp_values_; }
    const std::vector<MetricInfo>& power_metrics() const { return power_metrics_; }
    const std::vector<double>& power_values() const { return power_values_; }

    int device_count() const { return static_cast<int>(devices_.size()); }

private:
    void flatten();

    std::vector<std::unique_ptr<DeviceProbe>> devices_;
    std::vector<MetricInfo> temp_metrics_, power_metrics_;
    std::vector<double> temp_values_, power_values_;
};
