# mb-powermon-gui

A C++ / GTK4 (gtkmm) desktop GUI that monitors the **power and temperature** of
edge-AI NPUs, styled after Ubuntu's **System Monitor** (the *Resources* tab). It
is the GUI counterpart to the terminal-based [`mb-powermon`](../mb-powermon)
Python tool — same telemetry, rendered as scrolling time-series graphs in a
native desktop window.

![NPU Power and Temperature Monitoring GUI](assets/screenshot.png)

## What it shows

Two sections, each a scrolling 60-second graph with a per-device legend of live
values:

- **Power** (W) — one trace per power reading, 10 W default axis (auto-expands).
- **Temperature** (°C) — one trace per on-die sensor, fixed 0–100 °C axis.

Every metric from a given card shares that card's color, kept consistent across
both graphs, and the legend groups metrics **one device per row** (prefixed with
the PCIe BDF), e.g. `0000:c2:00.0 Axelera  SYS · AI0 · AI1 · AI2 · AI3`.

Each device row also carries a **per-device summary** between the name and the
individual readings: temperature shows the device's **average** across its
sensors (`avg 60°C`), power shows the **max** across its readings (`max 0.93 W`).
With one power source per card today the max equals that lone reading, but it's
computed over all of a device's power metrics — so when an external INA228 meter
is added alongside the on-card source, the label reports the highest of them.

## Supported devices & how telemetry is read

| Device | Temperature | Power | Passive? |
| ------ | ----------- | ----- | -------- |
| **Hailo-8** | `TS0` / `TS1` via HailoRT C++ API (`get_chip_temperature`) | `POW` — firmware-averaged, via `set/start/get_power_measurement` | temp ✅ · **power no** |
| **DeepX M1** | `T0`–`T2` via `dxrt-cli -s` (reads the kernel driver) | — (not exposed) | ✅ |
| **MemryX MX3** | `T0`–`T3` via sysfs/hwmon | `POW` via the MemryX SDK over the `mxa-manager` daemon | ✅ (daemon-shared) |
| **Axelera Metis** | `SYS` / `AI0`–`AI3` via `triton_trace --peek` | — (not exposed on M.2) | ✅ |
| **Qualcomm IQ** (IQ-9075 / QCS9075) | `N0-0`–`N1-2` via the `nsp-*-thermal` sysfs zones | — (**no** power measurement exists on the board) | ✅ |

Devices are auto-discovered at startup; only what's present appears. Whatever a
card doesn't expose simply doesn't get a trace.

### Why the Qualcomm IQ has no power row

Unlike the M.2 cards, the IQ-9075's NPU is the SoC's own Hexagon **NSP**, and the
board has no power measurement at all — not merely "not wired up":

- **No power-monitor IC on any IQ-9075 variant.** Scanning all 331 DTBs shipped
  with this kernel for INA/shunt compatibles, the only hits are `monaco`/`monza`
  (`ti,ina232` + `shunt-resistor`) — a different Qualcomm platform. None of the
  five IQ-9075 DTBs, mezzanine variants included, declare one. (Control: the
  `amc6821` fan controller *is* found in those DTBs, so the scan is sound.)
- **No undeclared one either.** Scanning i2c buses 18/19/20 turned up four
  devices missing from every IQ-9075 DTB, but none is a power monitor: `19-0x4c`
  is a TI **TMP411** board-ambient sensor (paired with the fan controller for fan
  control, not an NPU sensor), `20-0x47` is unidentified but answers none of the
  INA ID registers, and `19-0x51` / `20-0x21` fall outside the INA address range.
- **No hwmon power/current sensor** — the only real hwmon device under
  `/sys/devices` is that fan controller.
- **No power readback in the vendor runtime** — across 44 QNN libraries plus
  `libSNPE.so`, the only power symbols set or get the DCVS *mode*
  (burst / balanced / power_saver). That's a knob, not a meter.
- No `powercap`/RAPL, no `current_now` on the `power_supply` nodes, and the PMIC
  VADC offers die temperatures and `vph_pwr` *voltage* but no current.

The NSP also shares the package rail with the CPU and GPU, so even a
package-level figure would not be an NPU figure. The probe therefore reports
temperature only, rather than publishing an estimate that would sit next to
genuine measurements from the other cards.

**Adding real watts here**, if you want them, is easier than the external-meter
route: wire an INA23x/INA228 onto an i2c bus and declare it in a device-tree
overlay the way `monaco` does. This kernel already ships `ina2xx` / `ina238` /
`ina209`, so it shows up as an ordinary hwmon `power1_input` that a probe reads
in a few lines — no USB bridge and no Python helper.

The kernel exposes 12 NSP zones — 2 NSP instances × 3 blocks × 2 redundant TSENS
taps per block. The probe collapses each block's two taps to their **max**, so
the legend shows 6 entries (`N<instance>-<block>`) instead of 12.

### A note on "passive"

Everything is passive (never claims a device or perturbs another app's use of
it) **except Hailo power**: measuring it takes over the firmware's shared
averaging buffer and **disables Hailo's overcurrent protection while active**, so
it can interfere with — or be clobbered by — another HailoRT power client (e.g.
running the Python `mb-powermon` at the same time). Temperature reads are always
passive. Hailo power auto-recovers the buffer after repeated misses.

MemryX power has no C++ telemetry library, so it's read by a small persistent
Python helper launched from the MemryX venv, which connects once to the
multi-process `mxa-manager` daemon and streams the reading — no per-poll cost and
no interference with inference.

## Build & run

Requires `gtkmm-4.0`, CMake ≥ 3.16, and a C++17 compiler. The HailoRT runtime
(`libhailort` + `/usr/local/include/hailo`) is **optional** — without it CMake
prints `libhailort not found — building without Hailo-8 support` and compiles the
Hailo probe out, which is how the build works on hosts with no Hailo-8 (e.g. the
Qualcomm IQ-9075 EVK).

```bash
sudo apt install libgtkmm-4.0-dev cmake g++     # + the HailoRT runtime package
                                                # (optional; enables Hailo-8)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/mb-powermon
```

Quit by closing the window (or `Ctrl+C` in the terminal). Over SSH, prefix with
`DISPLAY=:0`.

### Configuration

- **MemryX power** uses a Python interpreter that can `import memryx`. It looks
  for `$HOME/mb-edgeai/memryx-env` by default; override with
  `MB_MEMRYX_PYTHON=/path/to/venv/bin/python3`. If none is found, MemryX simply
  shows no power row.
- The `[HailoRT] … overcurrent protection` lines printed at startup are the
  expected OCP-disable notice for the Hailo power session, not errors.

## Design

- **Sampling** runs once per second; each graph keeps a 60-second (61-point)
  history and draws newest-on-the-right. The per-device summary (avg temp / max
  power) is recomputed each tick, skipping any `NaN` readings.
- **Colors** come only from the project brand palette: device series take the
  **accent** colors in discovery order (Amber, Slate Blue, Sage, Plum, then Sand;
  Coral is reserved for alerts), so on a host with all backends present that runs
  Hailo → DeepX → MemryX → Axelera → Qualcomm IQ, and on a host with only one
  device that device is Amber. Graph chrome uses the **neutrals** (Slate Gray
  grid/text, near-white plot), and the title bar is the brand Teal.
- Missing readings render as gaps in the trace and `—` in the legend.

## Layout of the code

| File | Role |
| ---- | ---- |
| `src/Probes.{h,cpp}` | device discovery + per-tick telemetry (HailoRT / `dxrt-cli` / sysfs / `triton_trace` / MemryX helper / `nsp-*-thermal` zones). No GTK dependency. |
| `src/GraphArea.{h,cpp}` | reusable Cairo scrolling multi-series graph (fixed or auto axis, NaN gaps, height-responsive) |
| `src/MainWindow.{h,cpp}` | the two sections, device-grouped legend, teal header bar, 1 Hz refresh |
| `src/util.h` | brand palette (accent + neutral), size/rate formatting, nice-axis rounding |
| `src/main.cpp` | `Gtk::Application` entry point |

## License

Apache License 2.0 (matching the parent `mb-powermon` project).
