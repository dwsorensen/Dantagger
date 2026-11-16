# Dantagger (unified): agent instructions and project record

Last updated: 2026-11-16

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

- 2026-11-16: "add the ability to track coincidences between channels" in
  the Countrate panel: an **"Add coincidence countrate"** button adds a
  tracker of coincidences per second, plotted with the other countrates; the
  user picks the channels and a window in ps; a coincidence is a tag on one
  channel within the window of a tag on the other; "if [not in] the python
  library as a measurement class ... do so"; **the coincidence's channels
  need not be active in the countrate measurement**; "verify, then merge and
  push to remote". Done on branch `coincidence-countrate`, merged into
  `main` and pushed (see Decisions, "Coincidences").

- 2026-11-16: GitHub remote `origin` = `git@github.com:dwsorensen/Dantagger.git`
  (**public** repository; SSH key `~/.ssh/id_ed25519` is the user's
  account). Default branch renamed `master` -> `main`. Push only when asked.

- 2026-11-05: "Please switch to the new version ~/dws/Dantagger and remove
  the old one in SDE". Done: the SDE venv's switch was reinstalled from
  `tools/install_timetagger_hook.py` here (so `import TimeTagger` is this
  package), and `SDE/Dantagger` was moved to the desktop trash. (The old
  copy only spoke Release 1; with Release 3 on the board it failed with
  "response payload length 452 (expected 436)". Its folder also shadowed
  this package when a script ran from SDE, because the switch appends
  `python/` to `sys.path`.)

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
  (`SDE/Dantagger` no longer exists: removed 2026-11-05, see below.)
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
      **Superseded 2026-10-22 (later): release 2 default = 24 + 18 bits**
      (user: "I want 24, 18 to be the default"), after discussing the 6-bit
      index's re-anchoring at low rates.
- 2026-10-22 (later): "take a look at release 2 ... give me instructions" for
  the first hardware test: `docs/HARDWARE_TEST_R2.md`.
  11. **Neither the app nor Python resets anything on connect/open** (no
      trigger/hysteresis defaults applied; read back what the board has).
      Python still configures and calibrates the GPX2 on the first open after
      plug-in (that is setup, not a reset).
  12. Python package lives here (`python/Dantagger`). Since 2026-11-05 the
      SDE venv's `TimeTagger` switch points here (see below).
  13. `git init` here; commit at the end of the work session. The app is
      called **Dantagger**.
- 2026-10-22: **`icon.png` (repository root) is the app's icon "in this and
  future releases".** `build.py` embeds it at every app build (`icon_source()`:
  RGBA at 16-256 px; Pillow if available, else a pure-Python decoder that
  matches it); the app sets it on its main window and on measurements in
  their own windows; X11 class `Dantagger`. Replace the file to change it.
- 2026-10-22: **2D histogram zoom**: click and drag a region on the map and
  it is measured again to fill the view: adjust the bin widths and delays,
  never the number of bins. Axes stay 0 to max (delays not subtracted);
  shared delays are changed even if other panels use those channels; an axis
  that cannot move (STOP = START, or x STOP = y STOP) is fine as is; Back and
  Reset zoom; bin widths clamped to at least 1 ps; the zoom is not exactly a
  crop (first STOP after the START) and that is fine.
- 2026-10-22: "make that firmware change. Package it into release 3 and make
  sure it's super safe for my board" (compact 4-byte records), then "one
  thread receiving data and the other processing it" (done first). Answers:
  **`../FPGA/TDC_Firmware` is "firmly in-limits"** (editable; its released
  folders stay frozen); freeze Release 2 first and record the day's hardware
  results; **"Dantagger should speak release 3, don't worry about the older
  libraries"**; commit at the end. The board may be used for commands when
  the user says so ("You can use the board again"); loading the FPGA stays
  the user's.
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
- **Frame policy** in `dt::policy_gpx_config` (both the app and Python):
  release 2 24 + 18 bits (11.9 M events/s per channel at 250 MHz DDR, 2.1 s
  wrap), DDR unless the last calibration was SDR; release 1 24 + 20 SDR.
- **The setup follows the last successful calibration** (state file:
  divider code and `ddr`), so a release-2 fallback rate calibrated once in the
  app's TDC page survives power cycles and is used by Python too. **One state
  file per release** (`<serial>.json` release 1, as SDE/Dantagger;
  `<serial>.release2.json`): power cycles bring back release 1 from flash
  while release 2 is tested from RAM, and neither overwrites the other.
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

- **USB buffering (2026-10-22, user's first app session on release 2):**
  constant FPGA overflow warnings with ch 1-3 at high rate while the PC was
  saturated (SDE `tomography_bootstrap_gui.py` with 4 workers, 0 % idle,
  3 GB swap). Read-only diagnosis: USB 3 at 5 Gb/s; no app thread pegged;
  the tight spot was 8 x 4 KiB transfers in flight + the FPGA's 8192-record
  FIFO (~1 ms at full speed) that the D3XX library thread must keep serviced.
  Now 64 transfers in flight and a 4096-buffer library queue
  (`TdcOptions::usb_urb_count/usb_queue_buffers`, env `DANTAGGER_USB`,
  fallback to the defaults if refused); transfer size unchanged (4 KiB = one
  frame: a lone response still completes its transfer, no low-rate delay).
  `build/usb_bench` compares settings on the board. **Not yet measured on the
  board.**
- **Three threads per board (2026-10-22, user: "one thread receiving data
  and the other processing it").** `TdcBackend::run`: `dt-receive` only reads
  USB and checks frames (`Session::stream_batches`, one call per DATA frame)
  and hands each read's records to `dt-decode` through a 256 MB
  `RecordQueue`; `dt-decode` runs the taps (rate bar, recorder) and the record
  decoder and pushes chunks to the engine; `dt-engine` merges and measures.
  Threads are named (`top -H`). ThreadSanitizer: clean (run with `setarch
  $(uname -m) -R`, this kernel's ASLR otherwise breaks TSan). Board, live
  inputs, 2026-10-22: **23.35 M tags/s for 6 s, 0 dropped**; receive 56 %,
  decode 49 %, engine 61 % of a core (the single-thread design dropped at
  12.6 M/s). Link ceiling measured with `usb_bench`: ~28 M records/s
  (~225 MB/s, test pattern; 58 M pattern events, 0 wrong). Next limit: the
  engine thread (~35-38 M/s estimated).

- **Release 3 compact records (2026-10-22).** TDC_Firmware PROTOCOL.md 3.4:
  START flag bit 0, 4-byte words (event = stop 18 | channel 2 | ref low 12;
  EPOCH word = ref bits 23:12 when they change; markers HEAD/HI/LO; PAD to
  1007 words per frame). `tdcctl::Session::stream_batches` expands them
  (`CompactExpander`) to 64-bit records as they arrive (an event gets the
  last marker's coarse time), so the decoder, taps, recorder files and old
  tools see one format; `DecoderConfig::coarse_uncertainty_ps` (2^14 + 16
  ticks) widens the wrap budget and the watermark margin; sidecars carry
  `record_format` / `coarse_uncertainty_ticks` for the file backend.
  `TdcBackend` uses compact records automatically when
  `tdcctl::compact_problem` allows (release 3, 12-24 index bits, <= 18 stop
  bits; the 24 + 18 policy does); `TdcOptions::compact` / env
  `DANTAGGER_COMPACT=0|1`. `CompactPacker` = the FPGA packer in software
  (FakeFpga, tests; `rtl_frames` checks it word for word against RTL frames).
  Release 3 shares release 2's state file (same receive path). The app's TDC
  page shows the record format.

- **2D zoom** (`panel_hist2d.cpp`): left drag with auto-fit on; the STOP
  channels' delays move by the corner (x STOP = y STOP: both by the smaller
  corner, bins widened to cover the rectangle), bin width = span / bins
  rounded, >= 1 ps. A running panel stops, the delays go through
  `Device::set_delay`, and it starts again (cleared) once `Device::idle()`.
  Back / Reset zoom restore bin widths and delays (in memory, not in the
  session file).

- **Coincidences (2026-11-16).** Swabian `Coincidence` / `Coincidences` as
  virtual channels (`core/virtual_channels.hpp`), not a separate measurement,
  so every measurement can count them (Countrate, Counter, Correlation,
  Histogram, HistogramND, TimeTagStream) and Python scripts use the Swabian
  API. The engine runs the registered virtual channels over each merged
  block (before dispatch, engine mutex; nothing when none exist), in
  registration order, and inserts each tag **right after the tag that made
  it** (Swabian's order; sorting equal times by channel number broke chained
  coincidences on tied tags, found by the dense oracle stream). Semantics
  measured on Swabian 2.22.6 (14 hand cases + random/dense oracle streams):
  latest tag of every other group channel within `t - tag < window`
  (strict; same time counts); nothing used up; group = set; window 0 never
  fires. Channels 5-31 (`kMaxChannel`; measurement arrays are
  `kChannelSlots` = 32), lowest free number, never one still read by another
  virtual channel; a new run or an OverflowBegin clears the memory; only
  `CoincidenceTimestamp.Last`; start/stop switch one on/off, not part of
  SynchronizedMeasurements. App: the Countrate panel always counts 1-4
  (tiles show the chosen ones; accidental estimate `Σ R_i ∏ (1 - e^-R_j W)`
  from them) plus one virtual channel per coincidence (max 8 per panel);
  `Panel::make(dt::Tagger&)` creates them on the tagger the panel runs on
  (live or file replay) and `adopt_edited()` swaps them with the old ones;
  coincidences on a right-hand axis by default.

## Status (2026-11-16)

| Part | State |
|---|---|
| Copy, provenance, build (`build.py`) | Done; `docs/PROVENANCE.md`, `docs/copied_sha256.txt` |
| Host library tests (TDC_Firmware) | 166 checks PASS (also ASan/UBSan), with compact records (packer/expander round trip, bad streams, frames, FakeFpga runs, refusals) |
| Core C++ tests | 4,493,882 checks PASS (also ASan/UBSan): coincidence virtual channels (chunking invariance, run gap and overflow resets, numbering), release-3 (compact and 64-bit), release-2 and release-1 policy frames on the simulated FPGA, windows across run gaps, recorder -> file backend round trip (from run start, joined mid-run, time limit) |
| Python tests | 39 PASS, bit-exact against Swabian (oracle), coincidences included (`tests/test_coincidence.py`, oracle `test_coincidence_against_swabian`) |
| Benchmarks | as SDE/Dantagger within noise: end to end tomography set 55-59 M tags/s, HistogramND 360-372, decoder 85-99 M records/s (noisy). 2026-11-16 against `main` side by side: equal within noise except HistogramND 4 ps ~7 % lower, also with HistogramND's code reverted (code placement, not the change); Countrate 1-4 + 3 coincidences end to end ~28 M tags/s (loaded PC) |
| App | `build/app/dantagger`; `--selftest` 29 steps PASS (coincidences: 1+2 at 8 k/s within 1 ns, ~80/s within 100 ps, 1+3 at the accidental estimate, the Add button with channels 1 and 2 hidden), screenshots; `--snapshot` works against the simulator |
| Hardware | Release 2 on the board 2026-10-22: calibrated 250 MHz DDR, link ~28 M records/s, 23.35 M tags/s live with 0 dropped (TDC_Firmware `project_1/results/2026-10-22-release2-hw/`) |
| Release 3 | **Built and verified 2026-10-23, packaged** in TDC_Firmware `releases/release-3/` (tag `release-3`; BIN `ecc9ff7f…bad97f`): every build gate, full bench, RTL -> host cross-check with `build/rtl_frames` (271 frames, 33 compact, 0 failures), mutation 67/67. Host support here. **Not yet on the board** (open item 8) |

## Open items (need the user or hardware)

1. First hardware session: `build/app/dantagger --snapshot DIR` (STOP +
   STATUS, LCLKIN off if on), then Set up the board and a Countrate panel;
   Python `createTimeTagger()` twice (second: "setup skipped").
2. Release 1 with the 24 + 20 policy reads out at most 2.27 M events/s per
   channel; the SDE laser start runs at 2.73 MHz. That is why SDE/Dantagger
   used 16 + 18 (2.94 M/s). With release 1 and those scripts use
   `DANTAGGER_FRAME=16,18` (untested on the board) or release 2 (20.8 M/s).
3. ~~Release 2 decoding at low rates (6-bit index re-anchors)~~: resolved by
   the 24 + 18 default (simulated: no re-anchor over 30 s of silence at
   +100 ppm). The 6-bit frame remains an override and keeps its tests.
7. Release 2 bring-up on the board: `docs/HARDWARE_TEST_R2.md` (tdcctl with
   `--ref-bits 24 --stop-bits 18`, then the app and Python; fallback ladder in
   the app). USB 3 throughput (test pattern at full rate) still unmeasured.
4. Python scripts that relied on the 0.010 V / 1000 ohm defaults on open now
   get the board's values (a freshly powered board: -0.1004 V).
5. The SDE venv's `import TimeTagger` switch points here (2026-11-05). The
   user toggles it: `tools/install_timetagger_hook.py [--uninstall]`; check
   the live state with `.venv/bin/python -c "import TimeTagger;
   print(TimeTagger.__file__)"` rather than trusting notes.
6. Separate OS windows were checked only hidden (self-test); look at them on
   the desktop (XWayland).
8. Release 3 on the board: TDC_Firmware `project_1/README_HW_TEST_R3.md`
   (load to RAM, set up as release 2, `usb_bench --format both`, a compact
   stream, real signals; `DANTAGGER_COMPACT=0` falls back to 64-bit).

## How to work here

- Build and test: see README.md (`build.py`, `pytest tests`,
  `build/app/dantagger --selftest DIR`). Use the SDE venv's Python
  (`/home/qittlab/dws/SDE/.venv/bin/python`) for the module; one heavy job at
  a time.
- The app self-test sets its own XDG_CONFIG_HOME / XDG_STATE_HOME / data
  folder under DIR, so it never touches the user's settings.
- Tests that run SDE scripts use `PYTHONDONTWRITEBYTECODE=1` so nothing is
  written into SDE.
- `../SDE/Dantagger` is gone (removed 2026-11-05). `../FPGA/TDC_Firmware` may be changed
  (since 2026-10-22), never its `releases/` folders.

## Lessons

- 2026-10-22: a self-test that compares a frame-start snapshot with a live
  flag races (the scan finished mid-frame: "idle" but stale results). Use the
  snapshot for both.
- 2026-10-22: performance counters in a measurement's hot loop must live in
  locals; members alias the histogram writes.
- 2026-10-22: the decoder benchmark varies by ~15 % run to run on this PC;
  compare several runs before calling a regression.
- 2026-10-22: a record without its own coarse time (compact) must be
  anchored at the latest coarse time it could have, not the marker's: with
  latency jitter an earlier event can arrive after a later one and land before
  the run's base.
- 2026-10-22: `last_status()` during a run is the pre-START STATUS; check what
  a run did (record format, counts) after it ends.
- 2026-10-22: a Vivado build here can be killed by Claude Code's low-memory
  reaper (swap nearly full with the user's programs open); it is not a build
  failure. Check `free -m` first and do not restart it without asking.
- 2026-11-16: compare benchmarks against a build of `main` in a worktree
  (scratchpad), alternating runs; the PC's load moves single runs by 20 %+.
  A per-bin `fill()` of a channel-indexed array was a real 3x (Counter, 1 us
  bins) once the array grew to 32 channels; clear only the channels in use.
- 2026-11-16: doc edits by script must assert that the old text was found
  (a silent `str.replace` miss left the README's switch note stale).

## Change log

- 2026-10-22: Survey and plan; user answers. Repository created; both
  projects copied (hashes), FakeFpga merged, core ported to both releases,
  decoder budget, settings policy, no-reset-on-open, window-gap fix (with
  test), file backend + recorder (with tests), measurement diagnostics, the
  new app (docking, six panel types, setup pages, shared persistent channel
  settings, saved streams, self-test), docs. All tests pass. First commit.
- 2026-10-22: App icon from `icon.png` (embedded by build.py; all windows).
  Y axes with a 5 % margin around the data; calibration plot no longer capped.
- 2026-10-22: Receive / decode / engine threads (board: 23.35 M tags/s, 0
  dropped); `Session::stream_batches`; thread names; TSan clean.
- 2026-10-22: Release 2 calibrated at 250 MHz DDR on the board (user's app
  session, 12:50). FPGA overflows under a saturated PC: more USB transfers in
  flight and a larger library queue; `tools/usb_bench.cpp`.
- 2026-10-22: Release 2 default frame 24 + 18 (user); setup follows the last
  calibration's divider and SDR/DDR; one state file per release; app manual
  GPX2 inputs default to 24 + 18; tests (policy cases, 6-bit override cases,
  fallback survives power cycles and a release-1 session); the tdcctl part
  of the procedure rehearsed on the simulated FPGA; `docs/HARDWARE_TEST_R2.md`.
- 2026-10-22: Release 3 host support: compact records (protocol, session
  expansion, FakeFpga packer, decoder margin, sidecar keys, backend policy,
  tdcctl `--compact`, usb_bench `--format`, app records line, rtl_frames,
  tests); release 3 shares release 2's state file.
- 2026-10-22: 2D histogram zoom (drag a rectangle: bin widths and STOP
  delays, same bin counts; Back / Reset zoom); self-test step with a
  simulated drag.
- 2026-10-23: Release 3 verified and packaged in TDC_Firmware (build, full
  bench, rtl_frames cross-check, mutation 67/67; `releases/release-3/`);
  hardware procedure TDC_Firmware `project_1/README_HW_TEST_R3.md`.
- 2026-10-26: `artifacts/`: an interactive explainer of the firmware and
  host (index, fpga, host, formats pages + shared.css/js; Intuition /
  Mechanism / Exact detail levels; published as one claude.ai artifact).
  User asked for it after an FPGA-side measurement feasibility sweep
  (verdict: feasible except full-resolution 2D histograms; not pursued).
- 2026-11-05: SDE venv switch now points here; `SDE/Dantagger` removed
  (trashed), user request.
- 2026-11-06: SDE venv switch uninstalled (Swabian is the default again),
  user request.
- 2026-11-16: GitHub remote added (dwsorensen/Dantagger), branch renamed
  to `main`, first push (user request). `artifacts/.check/` (browser
  profile from the page checks) is ignored.
- 2026-11-16: Coincidences: `Coincidence` / `Coincidences` virtual channels
  in the core, Python (Swabian API, oracle-checked) and the app's Countrate
  panel ("Add coincidence countrate", accidental estimate, right-hand axis,
  CSV, session); `Panel::make(dt::Tagger&)`; tests (C++, Python reference
  and oracle, self-test steps); benchmark line. README stale switch note fixed.
