# Dantagger

Software for the TDC board (Alchitry Au V2 + Ft+ FT601 + comparator board +
TDC-GPX2 daughterboard; firmware in `../FPGA/TDC_Firmware`, releases 1 and 2).
One package with three faces over one C++ core:

- **The Dantagger app** (`build/app/dantagger`): every measurement type as a
  panel; any number running at once on the live stream or on saved tag
  files; panels as tabs, side by side, or in their own windows.
- **The Python library** (`python/Dantagger`): the Swabian-compatible
  `TimeTagger` replacement the SDE scripts use. Same API as
  `SDE/Dantagger`.
- **`tdcctl`** (`build/tdcctl`): the board's command-line tool from
  TDC_Firmware, unchanged.

```
FT601 (D3XX) ─ host/  tdcctl library: transport, protocol, session, calibration, simulated FPGA
            └─ core/  TdcBackend (both releases) → RecordDecoder → Merger → Engine → measurements
                      Countrate · Counter · Histogram · Correlation · HistogramND · TimeTagStream
                      FileBackend (saved tag streams) · StreamRecorder (raw record files)
     ├─ python/Dantagger   pybind11 module + the Swabian-style API
     └─ app/               Dear ImGui (docking) + ImPlot app, links the core directly
```

## Build

CMake is not installed on the lab PC; `build.py` drives g++. Run it with the
Python the module is for (the SDE venv's 3.12):

```sh
PY=/home/qittlab/dws/SDE/.venv/bin/python
$PY build.py               # Python module + tdcctl
$PY build.py --app         # + the app (build/app/dantagger, portable copy in build/app/dist/)
$PY build.py --tests       # + C++ tests (host library 146 checks, core 4.45 M checks)
$PY build.py --asan        # the C++ tests under AddressSanitizer + UBSan
$PY build.py --bench       # + build/bench
$PY -m pytest tests        # Python tests (27; bit-exact against the Swabian library where installed)
build/app/dantagger --selftest /tmp/dt-selftest   # the app against the simulated FPGA (26 steps, screenshots)
```

The app needs, to build: `libgl-dev libx11-dev libxrandr-dev libxinerama-dev
libxcursor-dev libxi-dev`. At run time only a desktop with an OpenGL driver
(X11 or XWayland). Another Linux computer: copy `build/app/dist/` (the binary
and `libftd3xx.so`); the FT601 needs the same udev rule as here; the file
dialogs use `zenity`. The core is compiled for x86-64-v3 (AVX2).

## The app

```sh
build/app/dantagger              # the board
build/app/dantagger --simulate   # a simulated board: 1 MHz laser on ch 4, detectors on 1 and 2 (31.55 / 31.75 ns), Poisson on 3
```

**Board.** Connecting changes nothing on the board. The sidebar and the
*TDC-GPX2 & LVDS* page show the firmware release and the capture format
(e.g. *Release 2 (design 0x54440002) · 24 + 18-bit frames (42 bits), DDR at
LCLKIN 250 MHz: at most 11.90 M events/s per channel*). If the board is not
configured and calibrated (after a power cycle), **Set up the board** does
what the Python library does on its first open: GPX2 configuration with the
frame policy, LVDS calibration, recorded in the same state file, so scripts
started later skip their setup. The manual GPX2 configuration, link test,
phase and LCLKIN controls of the TDC Control app are on the same page.

**Frame policy.** Release 2: a 24-bit reference index and 18 stop bits (1 ps
LSB), DDR: 11.9 M events/s per channel at 250 MHz, and a 2.1 s index wrap,
so times unwrap safely at any count rate. Release 1: SDR, 24 + 20 bits.
Python uses the same (`DANTAGGER_FRAME=REF,STOP` or `frame=` overrides it
there). The setup calibrates at the LCLKIN rate and data rate (SDR/DDR) of
the last successful calibration: a fallback chosen once on the TDC page (if
250 MHz DDR does not work on the board) stays in use, also after power
cycles and in Python. Each release has its own calibration state file
(`~/.local/state/dantagger/<serial>.json` for release 1, as SDE/Dantagger;
`<serial>.release2.json`), so switching releases loses neither.

**Channels (shared, persistent).** Trigger level, hysteresis, input delay and
dead time per channel are shared by every measurement and saved for the next
session (`~/.config/dantagger/channels.ini`). They can be changed while
measurements run: the board's run stops, the change is applied and streaming
resumes (the FPGA takes no comparator commands while it streams); the
measurements keep their data, and their capture time excludes the gap. If the
board's comparator differs from the saved values when the app connects (e.g.
after a power cycle) the app says so and offers *Apply the saved values* or
*Keep the board's values*; it never changes them by itself. Input delays use
the Swabian `setInputDelay` convention (added to every tag of the channel,
exactly, in software).

**Measurement panels.** *+ New measurement* (sidebar or the Measurements
menu): Countrate, Counter, Histogram, Correlation, 2D Histogram
(HistogramND), TimeTagStream. These are the C++ classes of the Python
library, so their numbers are the library's (bit-exact against Swabian).
Every panel has:

- **Source**: the live stream, or a saved tag file (read as fast as the
  computer can).
- **Start** (clears), **Continue** (keeps adding up), **Stop**, **Clear**;
  **Run for** *X* s of tag time (Swabian `startFor`), 0 = until Stop.
- **save raw tags** (off by default): while it runs, the unprocessed record
  stream (all channels, markers included) goes to the data folder as
  `NAME_YYYYmmdd_HHMMSS.bin` + `.bin.json`. The Python library never saves
  tags; only the app does, and only when asked.
- Settings (applied on Start, or *Apply* while running, which clears), the
  shared channel table, and the view:
  - **Countrate**: rate tiles (counts / capture, Swabian), spread, rate
    history plot, CSV.
  - **Counter**: rolling trace per channel, counts or rate, statistics per
    channel (last, mean, std, min, max, var/mean — 1 for Poisson), hover
    readout, CSV.
  - **Histogram** and **Correlation**: the TDC Control app's histogram plot:
    units, log y, auto-fit / zoom, hover readout, display merging, totals and
    warnings, live Gaussian fit (tallest peak or envelope of a comb) with
    errors and χ²/dof, CSV with the fit. Correlation also normalised (g2).
  - **2D Histogram**: the TDC Control app's 2D page: heatmap, log colour,
    colour maps, hover readout, display merging, starts / stops / pairs /
    outside / abandoned, the 1D projection at any angle (slider, arrows,
    keys) with its own CSV.
  - **TimeTagStream**: *Start streaming* writes the raw stream of the chosen
    channels for *Stream for* seconds of FPGA time (or until Stop), with the
    channel table open for trigger and hysteresis; shows the recording, the
    tags per channel, the newest tags, and the session's recordings with
    *open in a new Countrate / Counter / Histogram / Correlation / 2D
    Histogram* buttons. With a saved file as source it inspects that file.
- **Windows.** Every panel and setup page is a dockable window: drag a tab to
  dock it elsewhere, split, or pull it out into its own OS window. Presets
  (sidebar, Layout menu): **Tabs**, **Grid** (measurements side by side),
  **Windows** (each measurement in its own window). Right-click a measurement
  in the sidebar: show, own window / dock back, rename, duplicate, close.
  Panels, their settings and the layout are restored at the next start
  (`~/.config/dantagger/session.ini`, `imgui.ini`; `--reset-layout`).

Also from the TDC Control app: the **rate bar** (live rate per channel and
FPGA drops, whatever is running), **Comparator** (shared channel table, 1 s
count, LEDs, board status) and **Countrate vs trigger**.

The data folder is `data/` in this repository when the app runs from
`build/app/` (else `~/Dantagger-data`); File > Choose the data folder
changes it, `--data DIR` or `DANTAGGER_DATA` too.

## The Python library

```python
import sys; sys.path.insert(0, "/home/qittlab/dws/Dantagger/python")   # or PYTHONPATH
import Dantagger as TimeTagger

tagger = TimeTagger.createTimeTagger()      # first open after power-up: configure + calibrate (~1-2 s)
tagger.setTriggerLevel(1, 0.05)
sync = TimeTagger.SynchronizedMeasurements(tagger)
rate = TimeTagger.Countrate(sync.getTagger(), [1])
hist = TimeTagger.Correlation(sync.getTagger(), 1, 4, 10, 3000)
sync.startFor(int(1e12))
sync.waitUntilFinished()
print(rate.getData(), hist.getData().sum())
TimeTagger.freeTimeTagger(tagger)
```

The API, semantics and behaviour are those of `SDE/Dantagger` (see its
README; `docs/SWABIAN_API_USAGE.md`, `docs/GAMEPLAN.md`), with these changes
(user, 2026-10-22):

- **Opening changes no setting.** Trigger levels and hysteresis are read back
  from the board (`getTriggerLevel` returns what the board has; a freshly
  powered board reports −0.1004 V, DAC code 0). `reset()` still applies the
  defaults (0.010 V, 1000 Ω). The first open after power-up still configures
  the GPX2 and calibrates.
- **Both firmware releases**, with the frame policy above (release 1 now
  24 + 20 bits, the format proven on the board; it was 16 + 18; release 2
  24 + 18).
- `startFor(d)` now always covers exactly `d` of captured tag time: if the
  board's run restarts inside the window (a run that was still ending when
  the window began, or an autostarted measurement's settings change), the
  window's end moves out by the gap. Before, such a window ended short (a
  `Counter` bin of the window length then never completed; seen in the
  tomography flow test).

`import TimeTagger` → Dantagger: `switch/` and `tools/install_timetagger_hook.py`
point at *this* package. The SDE venv's switch still points at
`SDE/Dantagger` until it is reinstalled from here (not done; on request).

## USB buffering

The FPGA's event FIFO holds 8,192 records (under 1 ms at full USB 3 speed).
Dantagger keeps 64 USB transfers of 4 KiB in flight (TDC_Firmware's tools: the
D3XX default 8) and a 4,096-buffer library queue (default 256), so short
pauses of this computer (a busy CPU) do not overflow it. The transfer size
stays one FPGA frame, so there is no added delay at low rates.
`DANTAGGER_USB=URBS,QUEUE` overrides it (app and Python; `0,0` = the
library defaults); if the driver refuses the values the open falls back to
the defaults and says so (log, `info()`). `build/usb_bench` measures the
board's USB throughput under several settings (test pattern or `--live`,
`--load N` busy threads to simulate a loaded computer).

## Hardware status

Nothing in this package has run on the board yet. `SDE/Dantagger` never did;
release 2 has not been loaded. What is tested: the whole path against the
simulated FPGA of both releases (calibration, frames, decoding with ±95 ppm
clock drift and latency jitter, drops, fallback calibrations), the app's
self-test, and the measurements bit-exact against the Swabian library.

**Release 2 bring-up: `docs/HARDWARE_TEST_R2.md`.** For a quick look at a
board: `build/app/dantagger --snapshot DIR` (connects: STOP, STATUS, LCLKIN
off if on; screenshots of the Comparator and TDC pages).

## Files

| Path | What |
|---|---|
| `build.py` | the build (see above) |
| `host/` | TDC_Firmware's host library and `tdcctl` (copied; only `fake_fpga.hpp` changed) and its tests |
| `core/` | the Dantagger core: `tdc_backend` (board, both releases, frame policy, taps), `record_decoder`, `merger`, `engine`, `measurements`, `tagger`, `file_backend`, `recorder`, `sim_backend`, `python/module.cpp` |
| `python/Dantagger/` | the Python package (`_dantagger*.so` is built into it) |
| `app/` | the app: `main.cpp` (window, docking, sidebar, rate bar, layouts, self-test), `device` (board worker, shared channel settings, scan), `panel` (panel base, channel table), `panel_*.cpp` (the six types), `pages` (setup pages), `ui_common`, `rate_monitor`, `config` |
| `tests/` | Python tests, `tests/cpp/test_core.cpp`, the Swabian oracle |
| `bench/` | benchmarks |
| `switch/`, `tools/` | the `import TimeTagger` switch and its installer; the Swabian usage scanner; `usb_bench.cpp` |
| `third_party/` | FTDI D3XX 1.1.8, pybind11 3.1.0, Dear ImGui 1.90.9-docking, ImPlot 0.16, GLFW 3.4, portable-file-dialogs 0.1.0 |
| `docs/` | `HARDWARE_TEST_R2.md` (release 2 bring-up), `PROTOCOL.md` (firmware), provenance, the SDE Dantagger plan and API scan |
| `data/` | saved tag streams and exports (not in git) |
