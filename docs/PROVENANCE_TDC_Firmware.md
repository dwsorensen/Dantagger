# Files taken from the sibling projects

Sources:

- `../Comparator_Control`: the working tree of release 1 (BIN SHA-256
  `f4b8e36b…020da`), which the user reports working on hardware
  (2026-10-13). Its git history holds only the project-start commit
  `6c669b7`, so the file hashes below identify the copies.
- `../Test_project`: parent-repository commit `814fc87` (releases 9 and 10).

## Copied unchanged

| Here | Source | SHA-256 |
|---|---|---|
| `project_1/rtl/reset_supervisor.sv` | Comparator_Control (= Test_project) | `5c5bcbdd47045743416ce716f710311878c1f935930fe9730fc0b3edf0fb4d70` |
| `project_1/rtl/ft_clock_identifier.sv` | Comparator_Control (= Test_project) | `ad1e9bfc570c2dbc5574eaefbaf16427ef85f4b91871a82fc163484ec2a43b13` |
| `project_1/rtl/spi_master.sv` | Comparator_Control | `95515b18f4a1058e7b86e9f073cb60bbe8220ceaf3d28d1aa5a4e1f254df9af6` |
| `project_1/rtl/i2c_master.sv` | Comparator_Control | `585a73d3dc0de42b5f13346fc3b0512d9adec5f2b989a5edea4cef6b63f9ea9d` |
| `project_1/constraints/au_v2_pins.xdc` | Comparator_Control (= Test_project) | `4a7492b75e32fe87aaffa3e906b0b4c117391f1a374390696e3e77e4f9c337bb` |
| `project_1/constraints/ft601_66mhz.xdc` | Comparator_Control (= Test_project) | `a01c9c8c5171c76e53552878a0e5b6740fe9133c7d8e801d08474d52082ff677` |
| `project_1/constraints/comparator_pins.xdc` | Comparator_Control | `a2432381a8a4bb5b6edd8163b38a6d58c37999ffe4141dea473493ab05409875` |
| `project_1/sim/dac80504_model.sv`, `mcp23s08_model.sv`, `i2c_device_model.sv`, `i2c_monitor.sv` | Comparator_Control | `f4a4614d…`, `004d0b86…`, `132b8b5f…`, `fc60b5b1…` |
| `project_1/sim/ft601_fast_model.sv` | Test_project `814fc87` | `449b6ef354c0d9806f44621a315b6fc2d8dc6b8019d57e77413ab1d8a913f995` |
| `host/include/au2ft601/*.hpp`, `host/src/protocol.cpp`, `ft601_config.cpp`, `sha256.cpp`, `config_workflow.cpp`, `d3xx_common.*`, `d3xx_transport.*` | Comparator_Control (= Test_project; hashes as in `../Comparator_Control/docs/PROVENANCE.md`) | unchanged |
| `third_party/ftd3xx/` | Comparator_Control (FTDI D3XX 1.1.8; licence and hashes in its README) | – |

## Derived (modified copies)

| Here | Derived from | Changes |
|---|---|---|
| `project_1/rtl/board_ctrl.sv` | Comparator_Control `board_ctrl.sv` (`a1404680…6c4`) | TMP117: identification in PROBE/READBACK, automatic TEMP poll that never touches the comparator error fields, poll counters, 26 results, 7 device flags. Comparator steps unchanged. |
| `project_1/rtl/tdc_engine.sv` | Comparator_Control `cc_engine.sv` (`9e362515…053`) and Test_project `ft601_fast_66mhz.sv` (`9f960185…dec`) | Command receive, handshake and ownership unchanged; release-10 DATA slots, TXE gate and gap; DATA from the event FIFO; new commands, rules and run state. |
| `project_1/rtl/top_tdc.sv` | Comparator_Control `top_comparator_ctrl.sv` (`1aabaa4d…e4a`) | FT601 side unchanged; sys_ctrl, LVDS, event FIFO, GPX2 pins, LED map. |
| `project_1/sim/tb_board_ctrl.sv` | Comparator_Control `tb_board_ctrl.sv` (`a2476310…b9b`) | TMP117 model on the bus, poll checks, TMP117 fault case. |
| `project_1/sim/tb_tdc_top.sv` | Comparator_Control `tb_comparator_top.sv` | FT601 plumbing, corners and safety checks kept; everything else new. |
| `project_1/scripts/build_tdc.tcl` | Comparator_Control `build_comparator_ctrl.tcl` (`203181ee…f0d`) | Same FT601 and comparator gates; GPX2/LVDS gates, bus skew, port inventory. |
| `project_1/scripts/run_sim.py`, `mutate_tdc.py`, `check_pins.py` | Comparator_Control scripts | New benches, XPM library, new mutants, TDC netlist and package-file checks. |
| `host/src/tdc_session.cpp` | Test_project `session.cpp`, Comparator_Control `cc_session.cpp` | Frame scanner, threaded command write and read discipline kept; TDC streaming and calibration. |
| `host/src/tdcctl_main.cpp`, `host/gui/tdcctl_gui.py` | Comparator_Control `cmpctl_main.cpp`, `cmpctl_gui.py` | Same Backend/serve design; GPX2, LVDS, streaming commands and tabs. |

## Third-party libraries for the TDC Control app (downloaded 2026-10-20)

Release archives from GitHub; only the files listed were kept, unchanged.

| Here | Source archive | Archive SHA-256 | Licence |
|---|---|---|---|
| `third_party/imgui/1.90.9/` (core, `backends/imgui_impl_glfw.*`, `imgui_impl_opengl3*`, `fonts/Roboto-Medium.ttf`, `Cousine-Regular.ttf`, `binary_to_compressed_c.cpp`) | `github.com/ocornut/imgui/archive/refs/tags/v1.90.9.tar.gz` | `04943919721e874ac75a2f45e6eb6c0224395034667bf508923388afda5a50bf` | MIT (`LICENSE.txt`); fonts Apache 2.0 |
| `third_party/implot/0.16/` (`implot.h/.cpp`, `implot_internal.h`, `implot_items.cpp`) | `github.com/epezent/implot/archive/refs/tags/v0.16.tar.gz` | `961df327d8a756304d1b0a67316eebdb1111d13d559f0d3415114ec0eb30abd1` | MIT (`LICENSE`) |
| `third_party/glfw/3.4/` (`include/`, `src/`) | `github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz` | `c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01` | zlib (`LICENSE.md`) |
| `third_party/portable-file-dialogs/0.1.0/portable-file-dialogs.h` | `github.com/samhocevar/portable-file-dialogs/archive/refs/tags/0.1.0.tar.gz` | `db57f02a45b169613c41d1fb94edab5f03020cc5cdc5033f538c08b4982cd5d4` | WTFPL (`COPYING`) |
