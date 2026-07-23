# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

`mb-powermon-gui` is a C++17 / **gtkmm-4.0** desktop GUI that monitors the
**power and temperature** of edge-AI NPUs, styled after Ubuntu's GNOME **System
Monitor** (Resources tab). It is the GUI counterpart to the sibling Python TUI in
`../mb-powermon` (same telemetry, different presentation) — they share the theme
but **no code**.

The UI is deliberately simple: **two sections, Power and Temperature**, each a
scrolling 60 s time-series graph with a per-device legend of live values. It does
**not** show CPU/Memory/Network/Disk — an earlier iteration did (styled exactly
like System Monitor's resource graphs) but that was scrapped; if you find any
reference to `Sampler`, `Gauge`, or CPU/Mem panels, it's stale.

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
- **`GraphArea`** (`src/GraphArea.{h,cpp}`) — reusable Cairo `DrawingArea`.
  Percent / fixed-max (°C) / auto-scale (W, nice-rounded, `min_axis_max` floor)
  modes; axis labels at 0/25/50/75/100 %; newest sample on the right; NaN breaks
  the polyline into gaps. `vexpand` so graphs grow with the window.
- **`MainWindow`** (`src/MainWindow.{h,cpp}`) — builds the two `Gtk::Expander`
  sections, the device-grouped legend, the teal `Gtk::HeaderBar`, and the 1 Hz
  `Glib::signal_timeout` that pushes samples and updates labels.
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
- **Legend** is one row per device: `<bdf> <b>Name</b>` then the device's
  swatch+shortlabel+value entries in aligned grid columns (device prefix stripped
  from each label). Keep `value_labels_out` in metric order for the tick to
  update.
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
- **BDF** — Hailo from `scan()`; DeepX/MemryX/Axelera from
  `/sys/bus/pci/devices/*/vendor` (0x1ff4 / 0x1fe9 / 0x1f9d).

## Build / verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/mb-powermon                       # needs a display (X11/Wayland)
```

Requires `gtkmm-4.0` and the HailoRT runtime (`libhailort` + `/usr/local/include/hailo`,
found via `find_library`/`find_path` in CMake). No test suite.

To eyeball changes headlessly on X11: find the window and grab it —
`xwininfo -root -tree | grep mb-powermon`, then
`xwd -id <wid> -out w.xwd && ffmpeg -y -i w.xwd w.png`. **Kill instances with
`pkill -x mb-powermon`** (exact name) — `pkill -f build/mb-powermon` also matches
your own shell command and kills the launcher.
