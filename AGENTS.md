# Dantagger (unified): agent instructions and project record

Last updated: 2026-10-22

Read this whole file before working here, and keep it current in the same
change as the work (user instructions, decisions, status, blockers).

## What this is

One package for the TDC board (Alchitry Au V2 + Ft+ FT601 + comparator board +
TDC-GPX2 daughterboard; firmware in `../FPGA/TDC_Firmware`), combining:

- `../FPGA/TDC_Firmware/host`: the C++ board library (`tdcctl`, `au2ft601`)
  and the ImGui "TDC Control" app;
- `../SDE/Dantagger`: the Swabian-compatible Python library over a fast C++
  measurement core.

## User instructions

- 2026-10-22: Unify both projects here: one C++ backend that talks to the
  board; C++ libraries for live computation of every measurement type; a
  Python library exposing them "just like current Dantagger, don't really
  change it"; a standalone C++ measurement app (layout and design conventions
  of the TDC Control app, but a significant rewrite) that exposes every
  measurement type the Python library has, talking to the C++ components
  directly (no Python). Start, configure and stop any of them; several
  running at once; flexible window management (share one window, split into
  separate windows, or tabbed pages). **Every measurement panel at the level
  of detail of the old Live 2D Histogram page.**
- 2026-10-22: Copy/edit into this directory only. **Do not change
  `../FPGA/TDC_Firmware` or `../SDE/Dantagger`** (until the user says so).
- 2026-10-22, answers to the plan:
  1. Support firmware releases 1 and 2 (copy TDC_Firmware's working tree, which
     has the uncommitted release-2 host code).
  2. Python stays strict (`SettingsBusyError` while a user-started
     measurement runs). The app applies trigger/hysteresis/delay changes during
     acquisition by stopping the run, applying, and restarting.
  3. **Per-channel input delays only** (Swabian `setInputDelay` convention).
     No per-measurement "instant shift" delays and no view-margin machinery in
     the app (maybe later).
  4. Python's **never-save rule is unchanged**. The app: every measurement has a
     save option (default off) that writes the **unprocessed tag stream** into
     the data folder here (`data/`). Every measurement can run on the **live
     stream or on a saved tag file**.
  5. TimeTagStream panel: trigger/hysteresis configuration, "stream to disk
     for X seconds", start/stop.
  6. **Trigger levels and hysteresis in the app are persistent and shared
     across measurements.**
  7. Keep Countrate vs trigger, LED controls and the `tdcctl` CLI; drop the
     Tk GUI; **no StartStop** mode.
  8. Python and the app never run at the same time; each gives a clear error
     if the FT601 is busy.
  9. Docking: use the best docking library (Dear ImGui docking branch).
  10. Frames: release 2 = shortest safe frame (tdcctl's automatic choice);
      release 1 = 24 + 20 bits until 16 + 18 is tested on the board. **Show in
      the app which release is loaded and what the frames are** (config panel).
  11. **Neither the app nor Python resets anything on connect/open** (no
      trigger/hysteresis defaults applied; read back what the board has).
      Python still configures and calibrates the GPX2 on the first open after
      plug-in (that is setup, not a reset).
  12. Python package lives here (`python/Dantagger`); the SDE venv's
      `TimeTagger` switch keeps pointing at `SDE/Dantagger` until the user
      says otherwise.
  13. `git init` here; commit at the end of the work session. The app is
      called **Dantagger**.
- Carried over from both projects: hardware access is the user's (do not
  load the FPGA, run commands on the board only when asked); commit only when
  asked; one heavy job at a time (N95, 4 cores, 7.7 GB, shared machine; no
  CMake).

## Decisions (2026-10-22)

- **One board backend for both releases.** `core/tdc_backend.cpp` uses
  TDC_Firmware's release-2-aware host library (copied from its working tree).
  Each run takes the GPX2 configuration and clocking from a STATUS at its
  start (decoder: divisions, LSB, widths, coarse tick 10 ns / 8 ns, latency
  spread from `tdcctl::max_result_latency_us`) and refuses runs that
  `tdcctl::capture_problem` rejects.
- **Decoder for short reference indices.** The release-2 policy frame has a
  6-bit index (8 us wrap). The decoder chooses the wrap only while the
  prediction error (latency spread + clock-ratio uncertainty x gap) stays
  below 0.45 wrap, else re-anchors to the coarse clock; each re-anchor starts
  a new clock-ratio segment (before, a re-anchor could mix a wrong wrap into
  the ratio). Simulated: exact to the ps with +-95 ppm and 1 us jitter; one
  re-anchor after a 2 s silence.
- **Frame policy** in `dt::policy_gpx_config` (both the app and Python).
- **Opening applies nothing** (`Backend::current_trigger/hysteresis`,
  `Tagger::sync_from_backend`). Python's first open after power-up still
  configures and calibrates (setup, not a reset). The app connects with
  `SetupMode::none` and offers "Set up the board" (same state file).
- **App settings rule:** `SettingsPolicy::restart`; device commands that need
  an idle board (GPX2, calibration, scan, count) are refused while
  measurements run (`Tagger::exclusive(..., false)`); LEDs and comparator
  changes pause and resume the run.
- **Timed windows cover captured time.** `Engine::dispatch` moves a running
  window's end out by any gap between runs inside it. Found because SDE's
  tomography flow test failed about half the time (also with the original
  SDE/Dantagger): a window begun while the previous run was ending lost the
  gap, so a Counter bin of the window length never completed.
- **Saved tag streams = raw FPGA records** (`dt::StreamRecorder`, a
  `RecordTap` on the backend's reader thread with its own writer thread;
  `dt::FileBackend` decodes them like live data). Same format as
  `tdcctl stream` (FILE + FILE.json, extra sidecar keys: runs, comparator,
  channel mask). A recording that joins a run late gets a synthetic START, one
  that ends early a synthetic END. Python never registers a tap.
- **App measurement panels use the library's measurement classes** (no
  separate accumulators); per-panel delays and the old app's instant-shift
  windows were dropped (user answer 3); delays are per channel.
- Diagnostics added to Histogram/HistogramND are counted in locals and stored
  once per block (members aliased the histogram writes: -20 % on
  HistogramND).
- **2D panel limit** 4096 bins per axis, 1,048,576 in all (the library allows
  more; the app copies the data 10 times a second).
- Docking: Dear ImGui 1.90.9-docking (same release as before, ImPlot 0.16
  unchanged), multi-viewport on (separate OS windows through XWayland here).

## Status (2026-10-22)

| Part | State |
|---|---|
| Copy, provenance, build (`build.py`) | Done; `docs/PROVENANCE.md`, `docs/copied_sha256.txt` |
| Host library tests (TDC_Firmware) | 146 checks PASS (also ASan/UBSan) |
| Core C++ tests | 4,450,601 checks PASS (also ASan/UBSan): release-2 and release-1 policy frames on the simulated FPGA, windows across run gaps, recorder -> file backend round trip (from run start, joined mid-run, time limit) |
| Python tests | 27 PASS, bit-exact against Swabian (oracle) |
| Benchmarks | as SDE/Dantagger within noise: end to end tomography set 55-59 M tags/s, HistogramND 360-372, decoder 85-99 M records/s (noisy) |
| App | `build/app/dantagger`; `--selftest` 26 steps PASS (3 runs), screenshots; `--snapshot` works against the simulator |
| Hardware | **Nothing tested on the board** (release 2 not loaded yet; SDE/Dantagger never ran on hardware) |

## Open items (need the user or hardware)

1. First hardware session: `build/app/dantagger --snapshot DIR` (STOP +
   STATUS, LCLKIN off if on), then Set up the board and a Countrate panel;
   Python `createTimeTagger()` twice (second: "setup skipped").
2. Release 1 with the 24 + 20 policy reads out at most 2.27 M events/s per
   channel; the SDE laser start runs at 2.73 MHz. That is why SDE/Dantagger
   used 16 + 18 (2.94 M/s). With release 1 and those scripts use
   `DANTAGGER_FRAME=16,18` (untested on the board) or release 2 (20.8 M/s).
3. Release 2 decoding at low rates: before the clock ratio is measured (50 ms
   of events) a silence longer than ~10 ms re-anchors (an event placed by the
   coarse clock, ~1 us uncertain). Check on the board.
4. Python scripts that relied on the 0.010 V / 1000 ohm defaults on open now
   get the board's values (a freshly powered board: -0.1004 V).
5. The SDE venv's `import TimeTagger` switch still points at SDE/Dantagger;
   reinstall it from here (`tools/install_timetagger_hook.py`) only when asked.
6. Separate OS windows were checked only hidden (self-test); look at them on
   the desktop (XWayland).

## How to work here

- Build and test: see README.md (`build.py`, `pytest tests`,
  `build/app/dantagger --selftest DIR`). Use the SDE venv's Python
  (`/home/qittlab/dws/SDE/.venv/bin/python`) for the module; one heavy job at
  a time.
- The app self-test sets its own XDG_CONFIG_HOME / XDG_STATE_HOME / data
  folder under DIR, so it never touches the user's settings.
- Tests that run SDE scripts use `PYTHONDONTWRITEBYTECODE=1` so nothing is
  written into SDE.
- Do not change `../FPGA/TDC_Firmware` or `../SDE/Dantagger`.

## Lessons

- 2026-10-22: a self-test that compares a frame-start snapshot with a live
  flag races (the scan finished mid-frame: "idle" but stale results). Use the
  snapshot for both.
- 2026-10-22: performance counters in a measurement's hot loop must live in
  locals; members alias the histogram writes.
- 2026-10-22: the decoder benchmark varies by ~15 % run to run on this PC;
  compare several runs before calling a regression.

## Change log

- 2026-10-22: Survey and plan; user answers. Repository created; both
  projects copied (hashes), FakeFpga merged, core ported to both releases,
  decoder budget, settings policy, no-reset-on-open, window-gap fix (with
  test), file backend + recorder (with tests), measurement diagnostics, the
  new app (docking, six panel types, setup pages, shared persistent channel
  settings, saved streams, self-test), docs. All tests pass. First commit.
