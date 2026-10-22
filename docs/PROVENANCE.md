# Provenance

Copied 2026-10-22 from the two projects this package unifies (user: copy and
edit into this directory; the sources stay unchanged). The SHA-256 of every
original file is in `docs/copied_sha256.txt` (taken before any change).

- `../FPGA/TDC_Firmware` at commit `206091b` **plus its uncommitted working
  tree** (the release-2 host code, user answer 1). Its own provenance (files
  from Test_project and Comparator_Control, the app's third-party libraries)
  is kept in `docs/PROVENANCE_TDC_Firmware.md`.
- `../SDE/Dantagger` (no commits of its own; the SDE repository does not
  track it). Its provenance file is `docs/PROVENANCE_Dantagger.md`.

## From TDC_Firmware

| Here | Source | Changes |
|---|---|---|
| `host/include/au2ft601/*`, `host/include/tdcctl/*`, `host/src/*` (incl. `tdcctl_main.cpp`) | `host/include`, `host/src` | unchanged, except `host/include/tdcctl/fake_fpga.hpp` and `tdc_session.*` (below) |
| `host/include/tdcctl/tdc_session.hpp`, `host/src/tdc_session.cpp` | the same | `Session::stream_batches`: the sink is called once per validated DATA frame; the per-record bookkeeping tests the marker bit and decodes markers only. `stream()` keeps its per-record sink on top of it. |
| `host/include/tdcctl/fake_fpga.hpp` | `host/include/tdcctl/fake_fpga.hpp` (release 2) | SDE/Dantagger's changes to its release-1 copy merged in, in terms of the image's coarse tick: programmable `event_source` (true GPX2 times), `gpx_ppm` drift, `latency_ps` + `latency_jitter_ps` with completion-order records and in-flight carry-over, configured widths and REFCLK divisions, `drop_every` with OVERFLOW markers. The default (Poisson at `event_rate_hz`) is kept; TDC_Firmware's 146 host checks pass with it. |
| `host/tests/tdc_tests.cpp`, `rtl_frames.cpp`, `make_comb_run.py` | `host/tests` | unchanged |
| `app/_orig` (removed before the first commit) | `host/app` | reference while porting; the new app is in `app/` |
| `docs/PROTOCOL.md` | `PROTOCOL.md` | unchanged |
| `third_party/ftd3xx`, `implot/0.16`, `glfw/3.4`, `portable-file-dialogs/0.1.0` | `third_party/` | unchanged |

The app (`app/`) is new. Taken from the TDC Control app with changes:
`ui_common.cpp` (fonts, theme, cards, formatting, PNG writer; docking
colours, new widgets), `rate_monitor.cpp` (`RateMonitor`, as a record tap),
`pages.cpp` (the Comparator, TDC-GPX2 & LVDS and Countrate-vs-trigger pages
on the new Device), `panel_hist1d.cpp` (the histogram plot panel and Gaussian
fit display), `panel_hist2d.cpp` (the Live 2D Histogram page on HistogramND),
`device.cpp` (the trigger scan, the count).

## From SDE/Dantagger

| Here | Source | Changes |
|---|---|---|
| `core/*` | `src/core/*` | `merger.*`, `sim_backend.cpp`, `warnings.hpp` unchanged. `tdc_backend.*`: rewritten for both releases (LinkTiming, frame policy, decoder settings from each run's STATUS, setup modes, readback of the comparator, record taps, app access, busy-board error). `record_decoder.*`: coarse tick and latency spread from the configuration, 0.45-wrap budget, a new ratio segment at each re-anchor. `tagger.*`: settings policy (strict / restart), no settings applied on open, `exclusive()`. `engine.*`: timed windows extend over run gaps, `drained()`. `measurements.*`: diagnostics for the app (counted in locals), accessors. `backend.hpp`, `common.hpp`, `sim_backend.hpp`, `measurement.hpp`: the supporting additions. New: `file_backend.*`, `recorder.*`, `record_tap.hpp`. |
| `core/python/module.cpp` | `src/python/module.cpp` | frame defaults 0 (= the release's policy) |
| `python/Dantagger/__init__.py`, `timetagger_hook.py` | `__init__.py`, `timetagger_hook.py` | frame default and the open docstring |
| `tests/*`, `bench/*` | `tests/*`, `bench/*` | paths (`tests/conftest.py`), the open test rewritten (nothing applied on open), new C++ cases (release-2 and release-1 frame policies, windows across run gaps, recorder → file backend) |
| `switch/sde_timetagger_switch.py` | `switch/` | loads the package from `<repo>/python`; recognises a finder of another copy |
| `tools/*`, `docs/SWABIAN_API_USAGE.md`, `docs/GAMEPLAN.md` | same | unchanged |
| `third_party/pybind11` | `third_party/pybind11` | unchanged |

## Downloaded

| Here | Source archive | Archive SHA-256 | Licence |
|---|---|---|---|
| `third_party/imgui/1.90.9-docking/` (core, `imgui_internal.h`, `backends/imgui_impl_glfw.*`, `imgui_impl_opengl3*`, `fonts/Roboto-Medium.ttf`, `Cousine-Regular.ttf`) | `github.com/ocornut/imgui/archive/refs/tags/v1.90.9-docking.tar.gz` (2026-10-22) | `48e7e4e4f154ad98d0946126a84e2375f849f6a67792129a805817dd60a34330` | MIT; fonts Apache 2.0 |

The docking branch of the same release the TDC Control app used (1.90.9), so
ImPlot 0.16 is unchanged. It brings docking and multi-viewport (panels in
their own OS windows).
