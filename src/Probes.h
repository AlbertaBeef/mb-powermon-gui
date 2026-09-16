// Passive telemetry collection for the edge-AI NPUs on this host.
//
// "Passive" = never claims a device or perturbs another application's use of
// it: DeepX via `dxrt-cli -s` (reads the kernel driver), MemryX via sysfs/hwmon,
// Axelera via `triton_trace --peek` (reads the collector log, no Context claim),
// Qualcomm IQ via the `nsp-*-thermal` sysfs zones.
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
    // PCIe BDF, e.g. "0000:01:00.0". For an SoC-integrated NPU there is no BDF,
    // so this carries an equivalent locator instead (e.g. the SoC id "QCS9075").
    std::string bdf;
    // Name of another device whose color this one should share (empty = own
    // color). Set for a mapped INA228 so it reuses its accelerator's swatch.
    std::string color_alias;
};

// A single device's telemetry source.
class DeviceProbe {
public:
    virtual ~DeviceProbe() = default;
    virtual const char* name() const = 0;
    const std::string& bdf() const { return bdf_; }
    // Optional human-readable status when a device is present but not returning
    // data (e.g. firmware/runtime version mismatch, idle collector). Empty when
    // the device is nominal or genuinely absent.
    const std::string& note() const { return note_; }
    // Name of another device whose color to share (empty = own color).
    const std::string& color_alias() const { return color_alias_; }

    // Refresh readings. Fills temp_values()/power_values(), aligned to the
    // corresponding *_metrics() lists; a missing reading is NaN.
    virtual void poll() = 0;

    const std::vector<MetricInfo>& temp_metrics() const { return temp_metrics_; }
    const std::vector<double>& temp_values() const { return temp_values_; }
    const std::vector<MetricInfo>& power_metrics() const { return power_metrics_; }
    const std::vector<double>& power_values() const { return power_values_; }
    // Core clock (MHz). Its own family: a clock always belongs to the card
    // reporting it, so unlike the INA228 families there is nothing to fold,
    // and it follows plain discovery order in Probes::flatten().
    const std::vector<MetricInfo>& freq_metrics() const { return freq_metrics_; }
    const std::vector<double>& freq_values() const { return freq_values_; }
    // Accumulated energy (J) and charge (C) since discovery. Only the INA228
    // shunts have these — they are hardware accumulators integrating at the ADC
    // rate, not something derived from the 1 Hz samples.
    //
    // Two families rather than one, because a GraphArea carries a single unit
    // and formatter: joules and coulombs cannot share a plot. Energy is graphed;
    // charge is read but not plotted here.
    const std::vector<MetricInfo>& energy_metrics() const { return energy_metrics_; }
    const std::vector<double>& energy_values() const { return energy_values_; }
    const std::vector<MetricInfo>& charge_metrics() const { return charge_metrics_; }
    const std::vector<double>& charge_values() const { return charge_values_; }
    // Bus voltage (V) at the shunt. Only the INA228s have it. Its own family
    // because a GraphArea carries one unit, and because it must never join
    // power_: anything reading the max of that family as watts would see a
    // 3.3 and report it as power the card is not drawing.
    const std::vector<MetricInfo>& voltage_metrics() const { return voltage_metrics_; }
    const std::vector<double>& voltage_values() const { return voltage_values_; }
    // Current (A) through the shunt. Its own family for the same reason as
    // voltage: one unit per graph, and it must never join power_.
    const std::vector<MetricInfo>& current_metrics() const { return current_metrics_; }
    const std::vector<double>& current_values() const { return current_values_; }
    // Whole-system voltage / current / power from an inline supply meter (the
    // POWER-Z KM003C on the IQ-9075's USB-C input). Deliberately NOT part of
    // voltage_ / current_ / power_, which are the *accelerator* rails: same
    // units, different subject. A 19.9 V board input on the same axis as a
    // 3.3 V card rail flattens the sag that graph exists to show, and 22 W of
    // board draw does the same to a card's 0.85 W.
    const std::vector<MetricInfo>& sysvoltage_metrics() const { return sysvoltage_metrics_; }
    const std::vector<double>& sysvoltage_values() const { return sysvoltage_values_; }
    const std::vector<MetricInfo>& syscurrent_metrics() const { return syscurrent_metrics_; }
    const std::vector<double>& syscurrent_values() const { return syscurrent_values_; }
    const std::vector<MetricInfo>& syspower_metrics() const { return syspower_metrics_; }
    const std::vector<double>& syspower_values() const { return syspower_values_; }

protected:
    std::vector<MetricInfo> temp_metrics_;
    std::vector<double> temp_values_;
    std::vector<MetricInfo> freq_metrics_;
    std::vector<double> freq_values_;
    std::vector<MetricInfo> power_metrics_;
    std::vector<double> power_values_;
    std::vector<MetricInfo> energy_metrics_;
    std::vector<double> energy_values_;
    std::vector<MetricInfo> charge_metrics_;
    std::vector<double> charge_values_;
    std::vector<MetricInfo> voltage_metrics_;
    std::vector<double> voltage_values_;
    std::vector<MetricInfo> current_metrics_;
    std::vector<double> current_values_;
    std::vector<MetricInfo> sysvoltage_metrics_, syscurrent_metrics_,
        syspower_metrics_;
    std::vector<double> sysvoltage_values_, syscurrent_values_, syspower_values_;
    std::string bdf_;
    std::string note_;
    std::string color_alias_;
};

// Discovers every supported device and presents their metrics as four flat,
// stable lists (temperature, power, energy and charge). The lists are fixed
// after discover(); poll() only refreshes the aligned value vectors.
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
    // Core clock (MHz), plain discovery order — see DeviceProbe::freq_metrics.
    const std::vector<MetricInfo>& freq_metrics() const { return freq_metrics_; }
    const std::vector<double>& freq_values() const { return freq_values_; }
    // Accumulated energy (J) / charge (C) from the INA228 shunts, folded onto
    // the card each one measures. Empty on a host with no shunts.
    const std::vector<MetricInfo>& energy_metrics() const { return energy_metrics_; }
    const std::vector<double>& energy_values() const { return energy_values_; }
    const std::vector<MetricInfo>& charge_metrics() const { return charge_metrics_; }
    const std::vector<double>& charge_values() const { return charge_values_; }
    // Bus voltage (V), alias-ordered like power. Empty with no shunts.
    const std::vector<MetricInfo>& voltage_metrics() const { return voltage_metrics_; }
    const std::vector<double>& voltage_values() const { return voltage_values_; }
    // Current (A), alias-ordered like power. Empty with no shunts.
    const std::vector<MetricInfo>& current_metrics() const { return current_metrics_; }
    const std::vector<double>& current_values() const { return current_values_; }
    // Inline supply meter, whole board; see DeviceProbe.
    const std::vector<MetricInfo>& sysvoltage_metrics() const { return sysvoltage_metrics_; }
    const std::vector<double>& sysvoltage_values() const { return sysvoltage_values_; }
    const std::vector<MetricInfo>& syscurrent_metrics() const { return syscurrent_metrics_; }
    const std::vector<double>& syscurrent_values() const { return syscurrent_values_; }
    const std::vector<MetricInfo>& syspower_metrics() const { return syspower_metrics_; }
    const std::vector<double>& syspower_values() const { return syspower_values_; }

    int device_count() const { return static_cast<int>(devices_.size()); }

private:
    void flatten();
    // Emission order that keeps a mapped INA228 immediately before the
    // accelerator it names, so a folded reading joins that card's contiguous
    // run (the legend groups by runs of MetricInfo::device — see Probes.cpp).
    // Shared by every folded family, which is what lines the INA228 cells up in
    // the same legend column across graphs.
    std::vector<size_t> alias_device_order() const;
    // Accelerator index a PCIe-mapped INA228 should fold its reading onto, or -1.
    int pcie_merge_target(size_t k) const;

    std::vector<std::unique_ptr<DeviceProbe>> devices_;
    std::vector<MetricInfo> temp_metrics_, power_metrics_, freq_metrics_;
    std::vector<double> temp_values_, power_values_, freq_values_;
    std::vector<MetricInfo> sysvoltage_metrics_, syscurrent_metrics_,
        syspower_metrics_;
    std::vector<double> sysvoltage_values_, syscurrent_values_, syspower_values_;
    std::vector<MetricInfo> energy_metrics_, charge_metrics_, voltage_metrics_,
        current_metrics_;
    std::vector<double> energy_values_, charge_values_, voltage_values_,
        current_values_;
    // devices_ indices, emission order for every folded family.
    std::vector<size_t> dev_order_;
};
