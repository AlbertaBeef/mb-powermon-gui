# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

`mb-powermon-gui` is a C++17 / **gtkmm-4.0** desktop GUI that monitors the
**power and temperature** of edge-AI NPUs, styled after Ubuntu's GNOME **System
Monitor** (Resources tab). It is the GUI counterpart to the sibling Python TUI in
`../mb-powermon` (largely the same telemetry, different presentation) — they
share the theme but **no code**.

Window title: **"NPU Power and Temperature Monitoring GUI"**. Binary:
`./build/mb-powermon`. The **launcher label is different and deliberately so** —
`Name=mb-powermon` in the `.desktop`, because the descriptive title wraps to two
lines under a desktop icon. That label matches the sibling TUI's project name;
it is only a display string, and nothing dispatches on it.

Coverage is no longer identical: the GUI has a **Qualcomm IQ** (on-SoC Hexagon
NSP) probe that the TUI lacks, and both now read **INA228** external power over an
FT232H bridge (the GUI via **libftdi1** in C++, the TUI via Adafruit Blinka in
Python — no shared code). The GUI gained its own **PMD2** probe 2026-09-18,
ported from `mb-benchmark-gui` (which had it first) rather than from the TUI.
Port in either direction as needed — the two have no shared code, so it is a
reimplementation, not a move.

The UI is a **Graphs control frame** over **nine sections**, each a scrolling
time-series graph with a per-device legend of live values: System Voltage (V), System Current (A),
System Power (W), Accelerator Voltage (V), Accelerator Current (A),
Accelerator Power (W), Accumulated Energy (J), Temperature (°C), Frequency
(MHz).

**The titles are kept identical to `mb-benchmark-gui`'s**, units and all, so the
same graph reads the same way in both apps; that project adds three of its own
(Frame Rate, Efficiency, Energy) and shares every other title verbatim. Change a
title in one and change it in the other.

**Voltage and Current come before Power on purpose.** The INA228 *measures* VBUS
and the shunt drop and *derives* POWER as their product, so in this order a
sagging rail reads top-to-bottom: current rises, voltage falls, power is what
results. Both are collapsed by default — on a healthy supply they are flat lines
— and both exist only when INA228 shunts are present, as does Accumulated
Energy; over a 10-minute window a monotonic accumulator draws a
near-straight line whose *slope* is the average power the graph above already
shows, so its worth is the absolute total on the legend. Each
legend row also shows a **per-device summary** between the device name and its
individual entries: temperature takes the **max** across its sensors
(`max 60°C`) — the hottest die is what throttles, and a mean hides one die
running well above its neighbours — power takes
the max of its readings (`max 0.93 W`). It does **not** show
CPU/Memory/Network/Disk — an earlier iteration did (styled exactly like System
Monitor's resource graphs) but that was scrapped; if you find any reference to
`Sampler`, `Gauge`, or CPU/Mem panels, it's stale.

**Rail polarity is a config flag, not a code constant.** All four shunt breakouts
on the reference rig are wired IN+/IN- reversed, and the INA228 reports the shunt
drop *signed*, so `CURRENT` and `CHARGE` read negative for a card that is
drawing. `ina228.conf` takes a per-rail `invert` flag — `<port> = <label>,
invert` — applied in `INA228Probe::poll()` to those two families only. `VBUS`,
`POWER` and `ENERGY` come from unsigned registers and cannot carry a polarity
error, so inverting them would be wrong rather than merely redundant. Per-rail
because the harness can be corrected one breakout at a time.

**A power reading outside 0…1000 W is a sentinel, not a measurement.**
`plausible_power()` maps anything outside that band to NaN, at every parse site,
and the Python telemetry helper gates the same way before the value crosses the
pipe. An MX3 populated without power telemetry answers `0xFFFFFFFF` mW, which
this app reported as **4294967.29 W**. It is not merely an ugly graph: a legend
row's power summary is the **max** across that device's readings and skips NaN
but *not* a finite huge number, so the sentinel wins and becomes the card's
reported draw. Shared verbatim with `mb-benchmark-gui`, where the same value also
fed `power_for_device()` and corrupted every fps/W figure. **A card with no power
sensor must read NaN, never a number.**

**The bus-undervoltage latch is armed but not yet surfaced here.** `BUVL` is set
to 3.00 V with `ALATCH`, and a trip is detected in `poll()` — but this app has no
Logger and reads `note()` only at discovery, so the event currently goes nowhere.
mb-benchmark-gui pipes it to its CSV. Surfacing it in the Bus Voltage legend is
the obvious next step; until then the detection is live and the reporting is not.

The window title is **"NPU Power and Temperature Monitoring GUI"**.

Build with CMake; run `./build/mb-powermon` (needs a display).

## Architecture

Clean split between data and UI — keep it that way.

- **`Probes`** (`src/Probes.{h,cpp}`) — pure data, **no GTK include**. Discovers
  every supported device and presents their metrics as four flat, stable lists
  (`temp_metrics()` / `power_metrics()` / `energy_metrics()` /
  `charge_metrics()`), each with an aligned value vector refreshed by `poll()`.

  **Energy and charge are separate families on purpose.** A `GraphArea` carries
  one unit and one formatter, so joules and coulombs cannot share a plot: energy
  is graphed, charge is read but not plotted. They also must not be merged into
  `power_metrics_`, where the legend's per-device aggregate is a *max* — joules
  would overtake watts within seconds and make that summary meaningless.

  **`alias_device_order()` is shared by all four folded families** (it was
  `power_device_order()` when power was the only one). A folded metric takes its
  target's device index and the legend groups by *contiguous runs* of that index,
  so a folded reading emitted in discovery order — the FTDI probes are discovered
  last — would open a second legend row for a card that already has one. Using
  one order for every family is also what lines the INA228 cells up in the same
  legend column across graphs. A missing reading is `NaN`. Each `MetricInfo` carries
  `label`, `unit`, the owning `device` index, `device_name`, `bdf`, and
  `color_alias` — stamped in `Probes::flatten()`, not by the individual probes.
  Temp metrics follow discovery order; **power metrics use `power_device_order()`**,
  which groups a mapped INA228 just above the accelerator it names (INA228 first),
  so `power_values_`/`power_metrics_` share that order (`poll()` fills values via
  `power_dev_order_`, not raw device order). A mapped INA228 whose accelerator is
  present **and PCIe** (`pcie_merge_target()`) is *folded onto that device's row*:
  `flatten()` rewrites its metric's `device`/`device_name`/`bdf` to the accelerator's
  and its label to `<accel> INA228`, so it shares the row/color and the row's `max`
  spans on-die + shunt. Non-PCIe / absent-target / unmapped INA228 keep their own row.
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
  - **BoardThermalProbe** — board *ambient*, from a TMP401-family chip's hwmon
    `temp1_input` (TI TMP411 at i2c-19 0x4c on the IQ-9075). Kept a **separate
    DeviceProbe on purpose**: it is board temperature, not NPU die temperature, so
    it earns its own legend row and its own aggregate instead of dragging the
    NSP reading toward ambient. `bdf_` is the i2c locator (`i2c-19 0x4c`). Only the
    local channel is exposed — `temp2` (remote diode, ~65 °C, `temp2_fault=0`, so
    genuinely connected) is left out because what it measures is unknowable
    without the schematic. Nothing binds this chip automatically (absent from
    every IQ-9075 DTB); it needs a one-time `new_device` instantiation, and until
    then the probe finds nothing and contributes no row — verified.
  - **INA228Probe** — external reference-grade rail power from an **INA228** on an
    **FT232H** USB→I²C bridge (`libftdi1` MPSSE bit-banged I²C — behind
    `#if MB_HAVE_FTDI`, compiled out when libftdi1 is absent). One probe per
    bridge; `Probes::discover()` enumerates every `0403:6014` via sysfs and opens
    each by libusb **bus/address** (`ftdi_usb_open_bus_addr`). The `Ft232hI2c` +
    `Ina228` classes are lifted verbatim from `envic_ai_cpp`'s hardware-validated
    `mb_power_smoke`; defaults mirror the Python TUI (15 mΩ shunt, 5 A full-scale,
    canonical addresses `{0x40,0x41,0x44,0x45}`, one `POW` W metric per sensor).
    **POWER register is full 24-bit — no `>>4`** (unlike VBUS/VSHUNT/CURRENT). See
    gotchas for the identity problem.
- **`GraphArea`** (`src/GraphArea.{h,cpp}`) — reusable Cairo `DrawingArea`.
  Percent / fixed-max (°C) / auto-scale (W, `min_axis_max` floor) modes; axis
  labels at 0/25/50/75/100 %; newest sample on the right; NaN breaks the
  polyline into gaps. `vexpand` so graphs grow with the window.
  **`GraphArea::RangeMode` decides what the data does to the axis** — `Fixed`
  (baseline only, clips), `Max` (**default**, baseline until a reading reaches it
  then `kHeadroom` = 1.10 × peak, never shrinking back) or `Dynamic` (both ends
  track the data, margin = 10 % of the span). `axis_range(lo, hi)` returns both
  ends and `draw()` maps through them, so the bottom gridline is `lo`, not 0.
  `set_series_visible()` hides a series without discarding its history and drops
  it from the range calculation.

  **This app now drives all of it from `GraphControls`** — it used to expose no
  control and simply take the `Max` default. Keep `GraphArea.{h,cpp}` identical
  to `mb-benchmark-gui`'s (`cmp` after any change) rather than trimming or
  extending either copy alone.

  **The graphs keep 30 minutes and draw a window of it.** `kHistory` is
  `kMaxSpanSeconds + 1` = 1801 samples at 1 Hz — the top of the Time Range
  control — while `kSpanSeconds` (300) is only the window drawn by default.
  Buffering the maximum rather than the window is the point: narrowing the
  window discards nothing, so widening it again brings the older samples
  straight back. `GraphArea::view_window()` is the one place the two are
  reconciled, and **both `axis_range()` and `draw()` must use it** — scaling the
  axis over the whole buffer would let a peak from twenty minutes ago flatten a
  one-minute window.

  Consequence of the `Max` default: the °C axis is no longer pinned at 100. A die
  past 100 used to draw as a flat line on the top edge, indistinguishable from one
  sitting exactly at 100. The expanded top is deliberately **not**
  `nice_ceil`-rounded — quantizing 1370 up to 2000 would leave the trace in the
  bottom half of the plot, which is the readability problem the headroom rule
  exists to fix. (`nice_ceil` is consequently no longer called from `GraphArea`;
  it stays in `util.h`, which is shared verbatim and must not diverge.)
- **The header bar carries the panel toggle at its start**, the About button at
  its end — the same chrome as `mb-benchmark-gui`, added 2026-09-18. It collapses
  the control pane and gives its width to the graphs. `set_panel_visible()`
  hides **`side_`**, the Box that wraps `controls_` and carries its 12 px margin:
  hiding `controls_` alone would leave that margin as an empty strip. That is why
  this app needs the member where the sibling does not — there the panel *is* the
  Paned's start child. Hiding the start child is enough; GtkPaned gives the whole
  area to the remaining child and drops the handle, where detaching would
  re-parent live widgets and reset `set_position()`.

  **One icon on a `Gtk::ToggleButton`, deliberately not two swapped** —
  `sidebar-show-symbolic` is in Yaru and Adwaita both, while
  `sidebar-hide-symbolic` is **Yaru-only** and would render blank on stock
  Adwaita. **Ctrl+B** is the keyboard equivalent and the first keyboard shortcut
  in either app: a `Gtk::ShortcutController` on the window, scope MANAGED, because
  nothing holds the `Gtk::Application`. It flips the button, not the pane, so the
  two cannot disagree. The toggle is **per-session** — this app has no preference
  store of any kind.
- **`GraphControls`** (`src/GraphControls.{h,cpp}`) — the **Graphs** and
  **Telemetry** frames, ported from `mb-benchmark-gui`'s `ControlPanel`
  (2026-09-18). It is a `Gtk::Box` *of* frames, not one frame: Graphs is about
  which devices and how the axes behave, Telemetry about which instruments are
  drawn at all — the same split and the same two headings as the sibling. It sits in the
  **start child of a `Gtk::Paned`**, controls left and graphs right, the same
  arrangement as that app — the frame is the only thing in the pane here, where
  the sibling's carries the model lists and Start/Stop above the same frame.
  Three things about that pane are deliberate: the controls get
  `valign = START` (a frame stretched down the window is mostly empty box with
  its rows stranded at the top), `set_shrink_start_child(false)` so the handle
  cannot squeeze them below their minimum, and the default window grew to
  **1560x820** so the graphs keep roughly the width they had beside a ~620 px
  pane. The end child is **not** a `ScrolledWindow` — see the fill-layout note
  below. It builds **three** frames, in the same order as the sibling's panel so
  the two read alike: **Accelerators**, **Graphs**, **Telemetry**.

  **Accelerators** is one row per card — its name, then an `Enabled` switch.
  The sibling's rows carry that card's own controls after the switch; there is
  nothing to configure here, so the row is just the two. The names come from
  MainWindow, gated on `util::device_accent()` (see below), and the **whole
  frame hides itself** when no card was found rather than showing an empty box.

  **Graphs** has three rows, each a 12-character label so they align in a
  column:
  - **Values Range** — Fixed / Max / Dynamic, driving every graph at once.
  - **Time Range** — `Auto` (off by default) or a fixed 1-30 minute window
    (default **5**). Auto draws everything collected, so the traces fill the
    plot from the first sample instead of hiding off the right edge.
  The **Accelerators** switches are independent checkboxes, not a radio group:
  the point is comparing a chosen few on one plot, so "Hailo and Axelera, not
  the other two" has to be expressible. The sibling hardcodes its five cards from
  the `Accel` enum; there is no such enum here, so MainWindow passes the
  discovered device names that **`util::device_accent()`** recognises as cards —
  the same shared helper that gives each card its colour, which knows the five
  names (and the Qualcomm board's several spellings) while excluding the ambient
  probe.

    **The instruments are deliberately not in that section.** It was briefly called
    *Devices* and built from every discovered device name, which swept the PMD2
    into it; once Telemetry existed that was a **second control for the same
    thing**, able to contradict the switch the user actually reached for. The
    PMD2, POWER-Z and INA228 are governed by their own `Enabled` and nothing
    else. `device_shown()` returns true for anything with no checkbox, which is
    what hands them entirely to their own switch.
  - **Legends** — show/hide the per-device legend under every graph.

  **Telemetry** has one row per meter, in `discover()` order — **POWER-Z**,
  **PMD2**, **INA228** — which is also the order their graphs appear in.
  POWER-Z and INA228 get a bare `Enabled` switch each; they publish a handful of
  series apiece where the PMD2 publishes 34 and earns per-measurement boxes.
  **A row is built only where the instrument was found**, so nothing sits dead:
  on this x86_64 host INA228 and PMD2 show and POWER-Z does not.

  **INA228 is matched on the LABEL, not the device name, and that is
  load-bearing.** A mapped shunt is *folded* onto its card, so its `device_name`
  is `"Hailo"` and only the label still says so (`Hailo INA228 POWER`) — a
  device-name test would match none of the 20 shunt metrics on this host.
  Unmapped rails keep `INA228#<n>`, which the same substring test catches. The
  device filter has already had its say by then, so an instrument switch is an
  *additional* gate, not an alternative one.

  - **PMD2** — one checkbox per measurement *point* (not per series: ticking
    ATX12V governs its watts, volts and amps together), laid out in the meter's
    own tiers: `Enable` **alone** on the first line — it governs everything
    below it, and a reading on the same line would read as one more peer of the
    rails — then the summary tier (**TOTAL** and the three group subtotals),
    then the ten rails wrapped at five. `Enable` is a master switch
    that greys the rest rather than clearing them, so a chosen subset survives
    being switched off and back on. A host with no PMD2 gets no row at all.

    **The tiers are derived, not listed.** `MainWindow` reads them off two
    structural facts, so a firmware that renames or adds a rail needs no UI
    change: the TOTAL is the one power metric with **no family suffix** (so
    `legend_short()` leaves it untouched where it trims every other), and a
    **rail** is a point that also carries a voltage and a current — only rails
    are measured, a group is a POWER-only subtotal. Matching is on the exact
    trimmed name, which is what keeps the `EPS` group apart from the `EPS1` /
    `EPS2` rails, and `PCIE` from `PCIE1..3`.

  Everything it does is display-only: a hidden trace is hidden, never dropped,
  so the filter is retroactive and a hidden series is skipped by `axis_range()`.
  **Every filter hides the legend entry with the trace**, per *cell* since
  2026-09-18. It used to be per row, which sufficed while only the accelerator
  filter existed — that hides a card whole — but a Telemetry switch hides
  **part** of a row: turning INA228 off takes the shunt cell off a card that
  keeps its own sensors. `LegendRow` therefore carries `head` (device name +
  aggregate) and `cells`, each cell knowing the index of the metric it draws.
  The head follows the last surviving cell. All nine aggregates in `on_tick`
  likewise skip invisible series via one `counted(graph, k)` helper backed by
  `GraphArea::series_visible()`, or a row would report a max from a trace
  nobody can see.
  **Any new handler that reads another widget must go in `conns_`**, which the
  destructor disconnects — widget members die in reverse declaration order and a
  handler firing during teardown can otherwise touch one that is already gone.
- **`MainWindow`** (`src/MainWindow.{h,cpp}`) — builds the two `Gtk::Expander`
  sections, the device-grouped legend, the teal `Gtk::HeaderBar`, and the 1 Hz
  `Glib::signal_timeout` that pushes samples and updates labels. The shared
  legend builder (`build_metric_section`) optionally emits a per-device
  **aggregate** label (`AggEntry` = label + metric `[start,count)` range) after
  the device name; the tick fills it in — **mean** for temperature, **max** for
  power — skipping `NaN`. Power aggregates over *all* of a device's power metrics,
  which is exactly what makes a PCIe-mapped INA228 folded onto an accelerator's row
  raise that row's `max` — realized: a Hailo row shows both `POW` (firmware) and
  `INA228` (shunt), its `max` the larger; Axelera's lone `INA228` gives it its first
  power number.
- **Sensor readings are plausibility-gated.** `plausible_temp()` maps anything
  outside -40…150 °C to NaN at all three hwmon parse sites (MemryX, the Qualcomm
  NSP zones, the board ambient probe). A wedged MX3 publishes `65262000`
  millidegrees on every sensor — -274 °C read as unsigned 16-bit — and since the
  axis no longer clips, one such sample would re-top the temperature graph at
  ~72000 °C and squash every real card into the bottom pixel row. The Qualcomm
  site matters independently: it takes a `max` across redundant TSENS taps, so an
  ungated sentinel would always win. Don't "fix" this by clamping into range — a
  fabricated number beside genuine readings is worse than a gap.
- **`util.h`** — the brand palette (accent + neutral), size/rate formatting,
  `nice_ceil` (kept for reuse; `GraphArea` no longer calls it), and
  `make_palette` (returns **accent colors**, cycled).

To add a metric: extend a probe (or add a new `DeviceProbe`), then it flows into
the graphs/legend automatically — the UI is metric-agnostic.

## Conventions worth keeping

- **Passive by default.** Telemetry must not perturb another app's use of a
  device. The lone exception is **Hailo power** (user-approved): it claims the
  shared firmware buffer and disables OCP while active. Temperature is always
  passive. Don't make the other backends intrusive without a reason.
- **Per-device color.** Every metric of a device uses one color from the brand
  **accent** palette (`m.device` indexes `device_palette_`), consistent across
  both graphs. `MainWindow::colors_for()` maps metrics → colors. Each accelerator
  has a **fixed** colour from `util::device_accent()` — Coral = Hailo, Sage =
  MemryX, Slate Blue = DeepX, Amber = Axelera, Plum = Qualcomm — applied over
  `device_palette_` after `make_palette()`, so a card is the same colour here and
  in `mb-benchmark-gui` (`util.h` is shared verbatim; change it in both).
  Ordering subtlety: the fixed colour is applied **before** the INA228 alias
  pass, so a mapped shunt inherits its card's colour rather than a palette slot.
  The Qualcomm NSP names itself after the board (`IQ9075`, `QCS9075`, …), so
  `device_accent()` matches those forms too while excluding the separate
  `"<board> Board"` ambient probe (any name containing a space). A device may set
  `color_alias_` to another device's name to **share its swatch** (a mapped INA228
  reuses its accelerator's color); the ctor remaps `device_palette_` after
  `make_palette()` by matching `color_alias` → device name.
- **Colors come only from the brand palette** (`util::accent` / `util::neutral`).
  Accent = series (Coral, Sage, Slate Blue, Amber, Plum; Sand for fills — Coral
  is *not* reserved for alerts, it is Hailo's fixed colour). Neutral = graph
  chrome (Slate Gray grid/text, `#FAFAFA` plot bg —
  intentionally the original near-white, not pure white). Title bar = Teal via an
  app-scoped `Gtk::CssProvider`. Don't reintroduce ad-hoc RGB.
- **Legend** is one row per device: `<bdf> <b>Name</b>`, then the optional
  per-device aggregate (`max` for temperature and power, `avg` for frequency;
  a dim label at grid column 1), then the
  device's swatch+shortlabel+value entries in aligned grid columns (device prefix
  stripped from each label). Keep `value_labels_out` in metric order for the tick
  to update; the aggregate labels ride in a parallel `AggEntry` vector.
- **Legend cells wrap at `kMaxLegendCellsPerRow` (5), and the labels are
  shortened for display only.** A device with many metrics otherwise makes the
  grid far wider than the window; the graph inherits that width, and since
  `GraphArea` anchors its trace newest-at-the-right-edge, a young history lands
  off screen entirely and the graph reads as empty. `legend_short()` drops the
  family suffix (` POWER`, ` VBUS`, …) because each family has its own graph, so
  the suffix is redundant by construction. **Every wrapped cell joins the same
  `LegendRow`**, or the Devices filter would hide only part of a device.
- Refresh cadence / history are the `k*` constants at the top of
  `MainWindow.cpp` (`kIntervalMs`, `kSpanSeconds` = 300, `kMaxSpanSeconds` =
  1800, `kHistory = kMaxSpanSeconds + 1`, `kTempAxisMax = 100`). Power axis floor is `set_min_axis_max(10.0)`. Both are
  **baselines**: a reading that reaches either grows the axis to 10 % above the
  peak (`kHeadroom` in `GraphArea.cpp`).

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
  is found under `/opt/axelera/runtime-*/bin`. **Presence is keyed on the node, not
  on temps:** the probe registers `SYS`/`AI0`–`AI3` (NaN until data flows) whenever
  the node + tool exist, so a present-but-silent Metis stays in the UI — and `note_`
  (surfaced in the discovery log via `try_add`) says *why* it's silent. Two
  independent gates keep temps from appearing, both reset by a reboot:
  - **App firmware must be loaded.** The card loads its runtime firmware into RAM
    **on demand** (volatile — lost on reboot; nothing loads it at boot). Idle after a
    reboot it sits in **bootloader** firmware and `triton_trace`/`axcmd` refuse with
    `Version mismatch! Actual="v1.3.2+bl1" Expected="v1.7.0"` — the probe parses this
    into `note_`. Loaded by running any inference, or `axcmd --fwload
    /opt/axelera/device-*/omega/bin/start_axelera_runtime.elf` (RAM, **not**
    `--flashload`). `axsystemserver` (the `*:5555` broker) does **not** load it.
  - **Collector log level must be `inf`** — the firmware only logs `core_temps=` at
    `inf` (default `err`); `triton_trace --slog-level inf` flips it. That's a *global*
    level, so the probe stays passive and never does it — `note_` reads "collector
    idle" in this case.

  So temps "just work" while a Voyager app runs (it loads firmware **and** starts the
  collector) and vanish when the box is rebooted and left idle. Full recovery recipe:
  the `mb-axelera` skill's `references/runtime.md`.
- **ElmorLabs PMD2** — inline DC meter on the PSU harness, USB CDC
  (`0483:5740`), ported from `mb-benchmark-gui` 2026-09-18. `0x01` returns
  `{vid, pid, fw}`; `0x04` returns a 122-byte packed struct — 10 rails of
  `{int16 mV, int32 mA, int32 mW}` plus four **whole-watt** subtotals
  (EPS / PCIE / MB / TOTAL). It is a **SYSTEM** instrument like PowerZ, so every
  reading goes to `sysvoltage_` / `syscurrent_` / `syspower_` and the System
  graphs, never to a card's families.

  **The three families are what make the CSV-style labels unambiguous**: each
  metric is suffixed `POWER` / `VBUS` / `CURRENT`, because a rail labelled just
  `ATX12V` would be the same name in three families — the same collision the
  INA228 metrics avoid the same way. The GUI trims those suffixes for the
  legend only.

  Verified on this host 2026-09-18 through a headless `Probes.cpp` driver:
  14 power + 10 voltage + 10 current metrics, and the meter's own arithmetic
  checks out — EPS 112 W + MB 26 W = TOTAL 138 W, EPS1 61.4 + EPS2 50.3 ≈ EPS,
  and the four MB rails sum to 25.6 ≈ MB. Rails reading zero (HPWR1, PCIE1..3
  on an all-M.2 host) **are** published: a flat zero is a measurement, and
  suppressing them would make the metric set depend on what happened to be
  plugged in.
- **Qualcomm IQ** — the SoC still exposes no power sensor of its own, but the
  board can now be measured from outside it: a **ChargerLAB POWER-Z KM003C**
  inline on the USB-C supply, read by `PowerZProbe`. That is **whole-board**
  power (CPU + GPU + NSP + DRAM + peripherals), so it lives in the
  `sysvoltage_` / `syscurrent_` / `syspower_` families and its own **System**
  graphs — never in the accelerator families, where a 19.9 V input or 22 W of
  board draw would flatten the card traces beside it. Everything below still
  holds for the SoC itself: **there is no on-board power telemetry, and don't invent
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
  PCIe device, so the column carries the SoC id instead; INA228 (also no PCIe)
  carries its USB locator (`usb <bus>-<addr>`).
- **INA228 identity** — the FT232H bridges here have **no USB serial**, and every
  INA228 sits at the same default address `0x40` on its *own* bus, so neither the
  USB serial nor the I²C address disambiguates them. The stable key is the **USB
  port-path** (sysfs kernel name, e.g. `1-1`) — the physical port, unchanged by
  replug/reboot, *not* bus-devnum (`ftdi::Loc::port`; also the `bdf_`, shown as
  `usb 1-1`). `ina228.conf` (`$MB_INA228_CONFIG` → `$XDG_CONFIG_HOME/mb-powermon-gui/`
  → `~/.config/…`) maps `<port-path> = <label>`; a mapped bridge's probe is *named*
  `INA228 - <label>` (so its legend row reads e.g. `INA228 - Hailo`/`INA228 - Axelera`
  — the `INA228 - ` prefix is prepended in code, config values stay bare accelerator
  names) — this standalone name/color path applies only when the INA228 is **not**
  folded onto a PCIe accelerator (see `pcie_merge_target()` in the Probes section: a
  PCIe-mapped INA228 instead becomes an `INA228` entry on the accelerator's own row).
  An unmapped one is `INA228#<n>` by enumeration order. `bus`/`addr` (devnum) are
  still used to *open* the device (`ftdi_usb_open_bus_addr`) — only the identity/label
  keys on the port. Runtime prereqs: a udev rule
  (`ATTRS{idVendor}=="0403", MODE="0666"`) so the raw USB node is user-openable —
  it applies on the next `add` event (fresh boot / replug), not on already-
  enumerated devices — and libftdi auto-detaches `ftdi_sio` at open (so
  `/dev/ttyUSB*` being present is harmless). The `poll()` reads run on the GUI
  thread (~tens of ms for two sensors over MPSSE at 100 kHz); fine at this scale,
  move to a worker thread if many bridges are added.

## Desktop entry

**The desktop file is generated — edit `mb-powermon-gui.desktop.in`, not any
`.desktop`.** It carries `Exec=@MB_EXEC@`; CMake `configure_file`s it twice, into
`build/mb-powermon-gui.desktop` (points at `${CMAKE_INSTALL_FULL_BINDIR}`, and is
what `cmake --install` ships, alongside the binary and `assets/M_logo.svg`) and
`build/mb-powermon-gui.build.desktop` (points into the build tree, for running
uninstalled). This replaced a committed absolute `Exec=/media/…` path that was
one developer's machine and broken everywhere else — don't reintroduce a literal
path. `CMAKE_INSTALL_PREFIX` must be set at *configure* time, since `Exec` is
baked in then; `cmake --install --prefix` later would not match.

`mb-powermon-gui.desktop.in` uses `Icon=M_logo`, *not* the `M_benchmarking` that the
sibling `mb-benchmark-gui` uses — otherwise the two apps are indistinguishable in
the launcher. `Name` is **`mb-powermon`**, deliberately short: anything longer
wraps to a second line under the desktop icon (it was "NPU Power & Temperature
Monitor", then "mb-powermon-gui" — both wrapped). Keep the descriptive wording in
`GenericName`/`Comment`, which is what feeds the tooltip and menu search.
`Categories` is `System;Monitor;` only — a second *main* category makes the app
appear twice in the menu, which `desktop-file-validate` warns about. A launcher
in `~/Desktop` needs mode 755 **and** `gio set … metadata::trusted true`, or
GNOME's `ding` extension renders it as a text file; editing it in place rewrites
the file, so re-apply both afterwards.

## Known issues

- **`BrokenPipeError: [Errno 32] Broken pipe` traceback on exit.** Cosmetic and
  not this app's own code: the MemryX power helper subprocess writes to a pipe
  the parent has already closed, and Python prints the traceback to the stderr it
  inherited from us. **Fixed in `mb-benchmark-gui` and not yet ported here** —
  its helper calls `os._exit(0)` on write failure, which skips both the traceback
  *and* the interpreter's shutdown flush (a plain `break`/`sys.exit` still emits
  "Exception ignored"). `Probes.cpp` is shared between the two repos, so this is
  a one-line port from `../mb-benchmark-gui/src/Probes.cpp`.
- **The helper can outlive a hard kill.** `SIGTERM`ing the app leaves the venv
  `python3` helper orphaned and still holding the MemryX device; `prctl(
  PR_SET_PDEATHSIG)` in the child would close it. In `mb-benchmark-gui` this has
  a worse consequence (its binary links `libmemx`, whose ELF constructor then
  blocks in `memx_fops_open` *before* `main()`, so every subsequent launch
  hangs); this app links no MemryX library and is not exposed to that.

## Build / verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/mb-powermon                       # needs a display (X11/Wayland)
```

Requires `gtkmm-4.0`. Two backends are **optional**, each behind a CMake-set
define and compiled out when absent (so the build works on any host):
- HailoRT runtime (`libhailort` + `/usr/local/include/hailo`, via
  `find_library`/`find_path`) → `MB_HAVE_HAILO`, gates `HailoProbe`. Hosts without
  a Hailo-8 (e.g. the Qualcomm IQ-9075 EVK) build with it compiled out.
- `libftdi1` (the **1.x** dev package — `pkg_check_modules(FTDI ... libftdi1)`,
  header `<libftdi1/ftdi.h>`; **not** the legacy 0.x `libftdi-dev`) → `MB_HAVE_FTDI`,
  gates `INA228Probe`. `apt install libftdi1-dev`.

No test suite (the INA228 path was de-risked in `envic_ai_cpp/tests/mb_power_smoke.cpp`).

No display forwarding? These boards are aarch64 and may lack `ffmpeg`/ImageMagick;
`xwd` plus a ~10-line Pillow script that parses the XWD header is enough to turn a
window grab into a PNG (remember X stores truecolor as BGRX, so swap R/B).

To eyeball changes headlessly on X11: find the window and grab it —
`xwininfo -root -tree | grep mb-powermon`, then
`xwd -id <wid> -out w.xwd && ffmpeg -y -i w.xwd w.png`. **Kill instances with
`pkill -x mb-powermon`** (exact name) — `pkill -f build/mb-powermon` also matches
your own shell command and kills the launcher.
