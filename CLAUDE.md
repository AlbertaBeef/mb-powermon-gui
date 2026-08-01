# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

`mb-powermon-gui` is a C++17 / **gtkmm-4.0** desktop GUI that monitors the
**power and temperature** of edge-AI NPUs, styled after Ubuntu's GNOME **System
Monitor** (Resources tab). It is the GUI counterpart to the sibling Python TUI in
`../mb-powermon` (largely the same telemetry, different presentation) — they
share the theme but **no code**.

Coverage is no longer identical: the GUI has a **Qualcomm IQ** (on-SoC Hexagon
NSP) probe that the TUI lacks, and the TUI has external-meter probes (**PMD2**,
**INA228** over FT232H) that the GUI lacks. Port in either direction as needed —
but note the two have no shared code, so it is a reimplementation, not a move.

The UI is deliberately simple: **two sections, Power and Temperature**, each a
scrolling 60 s time-series graph with a per-device legend of live values. Each
legend row also shows a **per-device summary** between the device name and its
individual entries: temperature averages its sensors (`avg 60°C`), power takes
the max of its readings (`max 0.93 W`). It does **not** show
CPU/Memory/Network/Disk — an earlier iteration did (styled exactly like System
Monitor's resource graphs) but that was scrapped; if you find any reference to
`Sampler`, `Gauge`, or CPU/Mem panels, it's stale.

The window title is **"NPU Power and Temperature Monitoring GUI"**.

Build with CMake; run `./build/mb-powermon` (needs a display).

## Architecture

Clean split between data and UI — keep it that way.

- **`Probes`** (`src/Probes.{h,cpp}`) — pure data, **no GTK include**. Discovers
  every supported device and presents their metrics as two flat, stable lists
  (`temp_metrics()` / `power_metrics()`), each with an aligned value vector
  refreshed by `poll()`. A missing reading is `NaN`. Each `MetricInfo` carries
  `label`, `unit`, the owning `device` index, `device_name`, and PCIe `bdf` —
  stamped in `Probes::flatten()`, not by the individual probes.
  - **HailoProbe** — HailoRT C++ API (`libhailort`). `get_chip_temperature()` →
    TS0/TS1; power via `set/start/get_power_measurement`. `start_power()` calls
    `stop_power_measurement()` **first** to reclaim the DVM (the firmware runs its
    own periodic OCP sampling), then re-arms; auto-recovers after 3 missed reads.
  - **DeepXProbe** — shells out to `dxrt-cli -s`, regex-parses NPU temp lines.
  - **MemryXProbe** — temps from sysfs/hwmon (`name="memx0"`, `tempN_input`);
    power from a **persistent Python helper** (`fork`+`exec` of a memryx-venv
    interpreter) whose stdout is read non-blocking each poll. See helper notes.
  - **AxeleraProbe** — `triton_trace --device metis-0:<bdf> --slog --peek`,
    parses the last `core_temps=[...]`. **Peek only** — never enables the
    collector or opens a Context, so it can't race for device ownership.
  - **QualcommIQProbe** — the on-SoC Hexagon **NSP** (Qualcomm Dragonwing IQ /
    QCS, e.g. the IQ-9075 EVK). Temps from the `nsp-A-B-C-thermal` sysfs zones:
    A = NSP instance, B = block, C = one of two redundant TSENS taps. Each A/B
    pair collapses to its **max**, so 12 zones become 6 metrics labelled
    `N<A>-<B>`. Device name is parsed from `/proc/device-tree/model`
    (`… Addons IQ 9075 EVK` → `IQ9075`); `bdf_` carries the SoC id from
    `/sys/devices/soc0/machine` (`QCS9075`) since there is no PCIe device.
    **No power** — see gotchas.
- **`GraphArea`** (`src/GraphArea.{h,cpp}`) — reusable Cairo `DrawingArea`.
  Percent / fixed-max (°C) / auto-scale (W, nice-rounded, `min_axis_max` floor)
  modes; axis labels at 0/25/50/75/100 %; newest sample on the right; NaN breaks
  the polyline into gaps. `vexpand` so graphs grow with the window.
- **`MainWindow`** (`src/MainWindow.{h,cpp}`) — builds the two `Gtk::Expander`
  sections, the device-grouped legend, the teal `Gtk::HeaderBar`, and the 1 Hz
  `Glib::signal_timeout` that pushes samples and updates labels. The shared
  legend builder (`build_metric_section`) optionally emits a per-device
  **aggregate** label (`AggEntry` = label + metric `[start,count)` range) after
  the device name; the tick fills it in — **mean** for temperature, **max** for
  power — skipping `NaN`. Power aggregates over *all* of a device's power metrics,
  so a future INA228 reading joins the max automatically.
- **`util.h`** — the brand palette (accent + neutral), size/rate formatting,
  `nice_ceil`, and `make_palette` (returns **accent colors**, cycled).

To add a metric: extend a probe (or add a new `DeviceProbe`), then it flows into
the graphs/legend automatically — the UI is metric-agnostic.

## Conventions worth keeping

- **Passive by default.** Telemetry must not perturb another app's use of a
  device. The lone exception is **Hailo power** (user-approved): it claims the
  shared firmware buffer and disables OCP while active. Temperature is always
  passive. Don't make the other backends intrusive without a reason.
- **Per-device color.** Every metric of a device uses one color from the brand
  **accent** palette (`m.device` indexes `device_palette_`), consistent across
  both graphs. `MainWindow::colors_for()` maps metrics → colors.
- **Colors come only from the brand palette** (`util::accent` / `util::neutral`).
  Accent = series (Amber, Slate Blue, Sage, Plum; Coral reserved for alerts, Sand
  for fills). Neutral = graph chrome (Slate Gray grid/text, `#FAFAFA` plot bg —
  intentionally the original near-white, not pure white). Title bar = Teal via an
  app-scoped `Gtk::CssProvider`. Don't reintroduce ad-hoc RGB.
- **Legend** is one row per device: `<bdf> <b>Name</b>`, then the optional
  per-device aggregate (`avg`/`max`, a dim label at grid column 1), then the
  device's swatch+shortlabel+value entries in aligned grid columns (device prefix
  stripped from each label). Keep `value_labels_out` in metric order for the tick
  to update; the aggregate labels ride in a parallel `AggEntry` vector.
- Refresh cadence / history are the `k*` constants at the top of
  `MainWindow.cpp` (`kIntervalMs`, `kSpanSeconds`, `kHistory = span + 1`,
  `kTempAxisMax = 100`). Power axis floor is `set_min_axis_max(10.0)`.

## Per-device gotchas

- **Hailo** — `Device::scan()` returns the BDF (used as `bdf_`). The startup
  `[HailoRT] … overcurrent protection` lines on stderr are the expected
  OCP-disable notice, not errors. Running the Python `mb-powermon` in parallel
  will fail our power start with `DVM_ALREADY_IN_USE` — they fight over the same
  buffer; stop one.
- **MemryX** — the C++ `MxAccl`/`Client` telemetry classes are **not in a
  linkable `.so`** (only in the Python extension), hence the helper subprocess.
  Python import is ~4 s, so per-poll shell-out is impossible — the helper must be
  persistent. Interpreter search: `$MB_MEMRYX_PYTHON`, then
  `$HOME/mb-edgeai/memryx-env` (validated by a fast stat of its `site-packages/memryx`).
- **Axelera** — device name derives from the `/dev/metis-0:*` node; `triton_trace`
  is found under `/opt/axelera/runtime-*/bin`. Temps only appear if the collector
  is already running (we don't enable it — that would flip a global log level).
- **Qualcomm IQ** — **there is no power telemetry on this board, and don't invent
  one.** Measured on the IQ-9075 EVK, strongest evidence first:
  - **No power-monitor IC exists on any IQ-9075 variant.** Scanning all 331 DTBs
    under `/lib/firmware/<kver>/device-tree/qcom/` for INA/shunt compatibles, the
    only hits are `monaco`/`monza` (`ti,ina232` + `shunt-resistor`) — a different
    Qualcomm platform. None of the five `qcs9075-*iq-9075-evk*.dtb` (including the
    `-mezz` mezzanine variants) declare one. Control: `amc6821` *is* present in
    those same DTBs, so the scan is sound. Re-run it before trusting any claim
    that a rail sensor appeared.
  - No `hwmon` `power*_input`/`curr*_input`: the only real hwmon device under
    `/sys/devices` is the `amc6821` fan controller. **Search `/sys/devices`, not
    `/sys/class/hwmon`** — the latter is all symlinks and `find` won't descend
    into them without `-L`, which silently produces a clean-looking false pass.
  - No power readback in the vendor runtime: across 44 `/usr/lib/libQnn*.so` plus
    `libSNPE.so`, the only power symbols are
    `Snpe_SNPEPerfProfile_{Get,Set}PowerMode*` — the DCVS *mode*
    (burst/balanced/power_saver), a setting, not a measurement.
  - PMIC VADC exposes die temps + `vph_pwr` *voltage*, no current; no
    `powercap`/RAPL; `power_supply` USB-charger nodes have no `current_now`;
    debugfs `energy_model` covers only cpu0/cpu4.

  The NSP also shares the package rail with CPU/GPU, so even a package-level
  number would not be an NPU number. Temperature is the full extent of what this
  SoC offers.

  **If real watts are ever needed here**, the cheapest path is *not* the FT232H /
  INA228 route the Python TUI uses: wire an INA23x/INA228 onto an i2c bus and
  declare it in a DT overlay (`ti,ina232` + `shunt-resistor`, exactly as `monaco`
  does). The kernel already ships `ina2xx.ko` / `ina238.ko` / `ina209.ko`, so it
  lands as a standard hwmon `power1_input` that a probe reads in a few lines — no
  USB bridge, no Python helper.

  **The i2c buses have been scanned — there is no undeclared INA either.**
  `i2cdetect -y -r` on buses 18/19/20 (21/22 are DP AUX, not device buses) found
  four devices absent from every IQ-9075 DTB: `19-0x4c`, `19-0x51`, `20-0x21`,
  `20-0x47`. Only `0x4c` and `0x47` fall in the INA range, and neither is one:
  - `19-0x4c` is a **TI TMP411** (`0xFE`=0x55 TI, `0xFF`=0x12, 8-bit register map,
    `0x3E`/`0x3F` NACK; reg `0x00` reads 49 °C against `xo-therm` 48.2 °C). A
    board-ambient sensor paired with the `amc6821` on the same bus for fan
    control — **not** an NPU sensor, so deliberately not in the probe.
  - `20-0x47` answers 0x00 at `0x3E`/`0x3F`/`0xFE`; unidentified but definitively
    not a power monitor.
  - `19-0x51` / `20-0x21` sit outside 0x40–0x4F entirely (likely an EEPROM and a
    GPIO expander).

  An INA22x/23x always answers `0x3E` or `0xFE` with 0x5449 ("TI") as a 16-bit
  word — that is the check to repeat if this ever needs re-verifying. Needs
  `i2c-tools` and `device-tree-compiler`, both now installed on this host.
- **BDF** — Hailo from `scan()`; DeepX/MemryX/Axelera from
  `/sys/bus/pci/devices/*/vendor` (0x1ff4 / 0x1fe9 / 0x1f9d). Qualcomm IQ has no
  PCIe device, so the column carries the SoC id instead.

## Build / verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/mb-powermon                       # needs a display (X11/Wayland)
```

Requires `gtkmm-4.0`. The HailoRT runtime (`libhailort` +
`/usr/local/include/hailo`, found via `find_library`/`find_path`) is **optional**:
CMake sets `MB_HAVE_HAILO` only when both are found, and `HailoProbe` sits behind
`#if MB_HAVE_HAILO`. Hosts without a Hailo-8 — such as the Qualcomm IQ-9075 EVK —
build and run with the Hailo probe compiled out. No test suite.

No display forwarding? These boards are aarch64 and may lack `ffmpeg`/ImageMagick;
`xwd` plus a ~10-line Pillow script that parses the XWD header is enough to turn a
window grab into a PNG (remember X stores truecolor as BGRX, so swap R/B).

To eyeball changes headlessly on X11: find the window and grab it —
`xwininfo -root -tree | grep mb-powermon`, then
`xwd -id <wid> -out w.xwd && ffmpeg -y -i w.xwd w.png`. **Kill instances with
`pkill -x mb-powermon`** (exact name) — `pkill -f build/mb-powermon` also matches
your own shell command and kills the launcher.
