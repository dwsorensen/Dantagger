# Third-party and copied code

Copied 2026-10-21 for Dantagger (user: "The host code should be copied into ./Dantagger").

## `tdcctl/`: TDC_Firmware host library

Source: `/home/qittlab/dws/FPGA/TDC_Firmware/host` (firmware design `0x54440001`, release 1). That repository has no commits yet, so the SHA-256 of each original file identifies the copy. Its `host/README.md` and `docs/PROVENANCE.md` say where these files came from (Test_project, Comparator_Control). Only the files Dantagger needs were copied (not the CLI, GUI, app, run files, histograms or fits).

| File | Original SHA-256 | Changes |
|---|---|---|
| `include/au2ft601/config_workflow.hpp` | `30fe2b75980c6afe8c6f7d8e89ca4d47bb718345e690c795b924d447fd4611b6` | unchanged |
| `include/au2ft601/ft601_config.hpp` | `02aae76c8cf734ce96d352c1e5613839385629c5484b5d7d3cd0a4dc4c30bc23` | unchanged |
| `include/au2ft601/protocol.hpp` | `1f14734f16e626a2199857af1c245dd6dd4042a521f3940720cf09de2631990a` | unchanged |
| `include/au2ft601/sha256.hpp` | `b6791f866e095da836c8c8a15b1232f5362ea5116cc165d69fdeac294381532f` | unchanged |
| `include/au2ft601/transport.hpp` | `fbd5105b441385cb3b5d43369a4c09c73951eb2d6734ede9306c45b2f7d7c9d8` | unchanged |
| `include/tdcctl/fake_fpga.hpp` | `1d3d04327a4a7122b8a3292152eb032ac6821e9bf427d077cd29d2ce5d315364` | **modified**, see below |
| `include/tdcctl/tdc_protocol.hpp` | `c82f3481385cdcd5973ba9bd8c09e2c6d2808e483bf0e3df8a50f11344ff6274` | unchanged |
| `include/tdcctl/tdc_session.hpp` | `037c475b68e52f87704264c95b95894d7458d64377e4feecc20e42bd0c67ece5` | unchanged |
| `src/config_workflow.cpp` | `14dde0c22afd0626dfe936d5c1fffa6cc0e57ad9d80ba0765fe4ff69af3c5c6b` | unchanged |
| `src/d3xx_common.cpp` | `14666dc5baf39d313bbee40f87eef9313157cbef22100ce8fadfee38aad23ddc` | unchanged |
| `src/d3xx_common.hpp` | `9f6de91150b3031a3532b1d596281beb3af82269910706807d2d139d20c22227` | unchanged |
| `src/d3xx_transport.cpp` | `31c9cf5c67191af4585857c07e7a13cb8114e5e556734cc29bcf7814f787114e` | unchanged |
| `src/d3xx_transport.hpp` | `fb7d5931bf6b03cf8005d5933edf9450e4a0fae51dd2075f312a76a59e12fc78` | unchanged |
| `src/ft601_config.cpp` | `b919e932544606dbdc4dd99f7a4ac93682254cf15bb4d1ee344370617dc46dd8` | unchanged |
| `src/protocol.cpp` | `f737a25b30450fb84d00bb8d211733fcbf7630db9425d3ff229aee6a3bf6c9e0` | unchanged |
| `src/sha256.cpp` | `046abafb84c87d60ccd49cad6c88a1929bb830c202df2d622269ddf9498d7651` | unchanged |
| `src/tdc_protocol.cpp` | `26862090eb819ce454745aefbbc9bf740b422b453085b0b6966fd4966cc8e374` | unchanged |
| `src/tdc_session.cpp` | `f45c9a5acae17e756734b62f6687805815510cd58b143b1798663335a7d2f91d` | unchanged |

Modifications to `include/tdcctl/fake_fpga.hpp` (the simulated FPGA, used
only by tests and `createTimeTagger(simulate="fpga")`): `advance()` takes
events from a programmable `event_source` (true GPX2 times), models GPX2↔coarse
clock drift (`gpx_ppm`), readout latency (`latency_ps`, `latency_jitter_ps`)
with completion-order records and in-flight carry-over between steps, encodes
the configured reference-index/stop widths and REFCLK divisions, and can drop
every Nth event at the FIFO with OVERFLOW markers (`drop_every`). The original
generated Poisson events with fixed 24-bit/125000 encoding; that remains the
default when `event_source` is empty.

## `ftd3xx/`: FTDI D3XX 1.1.8

Copied unchanged from `TDC_Firmware/third_party/ftd3xx` (its README, kept
here, records the archive, hashes and licence terms).

## `pybind11/`: pybind11 3.1.0 headers

`include/` and `LICENSE` (BSD-3-Clause) from the PyPI wheel
`pybind11-3.1.0-py3-none-any.whl`, SHA-256
`b8488090f8acffbcb6b5d6a85571a6827a0a2981ffb75e5a0b27b87c4a6b7dd0`,
downloaded 2026-10-21; unchanged.
