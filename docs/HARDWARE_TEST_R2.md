# Release 2 on the board, with Dantagger

How to bring up TDC_Firmware Release 2 (`2_tdc`, design 0x54440002) and
check it with this package. The firmware side (image, hashes, loading,
safety) is TDC_Firmware's `project_1/README_HW_TEST_R2.md`; read its
**Safety** section first. This procedure follows it with one difference: the
GPX2 frame is Dantagger's Release 2 policy, **24 + 18 bits DDR** (11.9 M
events/s per channel at 250 MHz), not TDC_Firmware's shortest frame
(6 + 18). `tdcctl` is TDC_Firmware's tool unchanged, so its own default is
still 6 + 18: **pass `--ref-bits 24 --stop-bits 18` to every `gpx-config`
and `calibrate`** below.

Only one program can use the board at a time: close the app before `tdcctl`
or Python, and the other way round.

```sh
cd /home/qittlab/dws/Dantagger
T=build/tdcctl
F="--ref-bits 24 --stop-bits 18"
```

Write down (or `| tee`) every step's output; TDC_Firmware keeps hardware
results in `project_1/results/<date>-release2-hw/`.

## A. Firmware bring-up (tdcctl)

RJ45 unplugged for A1-A4, as in TDC_Firmware's procedure.

**A1. Load to RAM, status.** Release 1 stays in flash: a power cycle brings it
back.

```sh
sha256sum ../FPGA/TDC_Firmware/project_1/build/artifacts/2_tdc.bin   # 4696c133...33e6
alchitry load --ram --board AuV2 --device 0 --bin ../FPGA/TDC_Firmware/project_1/build/artifacts/2_tdc.bin
$T status
```

Expected: TDC firmware `0x54440002` (release 2), comparator devices and
TMP117 OK, GPX2 not configured, `LCLKIN off (high impedance) (divider 0:
250.00 MHz)`, `MMCM locked, IDELAYCTRL ready`.

**A2. GPX2 configuration.**

```sh
$T gpx-config $F
```

Expected (checked on the simulated FPGA):

```
GPX2 registers: 3F 0F 35 48 E8 01 C0 53 A1 13 00 0A CC CC F1 7D 00 (... ref index 24 bits, stop 18 bits, divisions 125000, DDR ...)
GPX_CONFIGURE: OK
    readback: 3F 0F 35 48 E8 01 C0 53 A1 13 00 0A CC CC F1 7D 00
  LVDS: ... frames 24+18 bits DDR (1 samples per bit)
```

Register 2 = **0x35** (DDR, 18 stop bits, 24 index bits); readback identical.

**A3. Calibrate at 250 MHz DDR.**

```sh
$T calibrate $F | tee calibrate_250ddr.txt
```

Expected: `calibrating: GPX2 test pattern, LCLKIN 250.00 MHz DDR, ...;
frames 24+18 bits`, then `calibration OK: bit phase 0, phase P (... ps into
the 2000 ps bit; W ps error-free)`.

- **Pass:** W well above zero. At 2 ns per bit, about 1 ns or more is
  comfortable (Release 1 at 10 ns had 9.4 ns error-free). The simulator says
  1714 ps, but it has no jitter, so it is no guide.
- **Fail** (no error-free point, or errors in the final check): go to part D.
  Nothing is harmed.

**A4. Test pattern at full rate.**

```sh
$T gpx-config --test-pattern $F
$T stream --out tp250.bin --duration 1
$T decode tp250.bin --test-pattern
$T gpx-config $F
```

- `decode --test-pattern` must say **TEST PATTERN OK**: every event equals
  ref `0xF0CCAA`, stop `0x2ACF0` (the pattern cut to 24 + 18 bits). Any
  differing event is a sampling or decoding error: stop and send me the
  output. (The simulated FPGA does not send the pattern, so this step could
  not be rehearsed.)
- The GPX2 then sends 4 x 11.9 M = 47.6 M events/s, more than USB carries:
  expect most dropped by the FPGA (`FPGA dropped ...`). The received
  M events/s is the USB 3 link's real throughput. Please record it: the
  ~31 M records/s estimate has never been measured (Release 1 on USB 2 gave
  6.0 M/s).
- Last command: the normal configuration back.

**A5. Temperature.** `$T status`: note the TMP117 reading (it rose from 28 to
35 °C during the first Release 1 session).

## B. The Dantagger app

**B1. Start it.**

```sh
build/app/dantagger
```

The sidebar shows **Release 2 (design 0x54440002)** and **Board not set up**:
Dantagger has no record of tdcctl's calibration, and keeps its own per
release (`~/.local/state/dantagger/<serial>.release2.json`).

**B2. Set up.** Click **Set up the board**. The log should say `board set up:
TDC-GPX2 configured (... ref index 24 bits, stop 18 bits ... DDR ...), LVDS
calibrated (LCLKIN 250 MHz, bit phase 0, phase step P, error-free window W ps
of 2000 ps)`. On the *TDC-GPX2 & LVDS* page:

- Capture: `24 + 18-bit frames (42 bits), DDR at LCLKIN 250 MHz: at most
  11.90 M events/s per channel`.
- Frame policy: in use.
- State: configured and calibrated.

**B3. Channels.** Loading the FPGA resets the comparator (DAC code 0 =
−0.100 V), so the app reports that the board differs from your saved values,
and changes nothing. Enter your usual triggers and hysteresis in the channel
table on the *Comparator* page (e.g. your 1 kHz pulses on STOP1/2 last time:
0.050 V, 75 Ω; remember that at 75 Ω a 0.010 V trigger never resets), or
**Apply the saved values**. Plug the RJ45 back in.

**B4. Rates.** Use **+ New measurement → Countrate** on all channels, then
Start.

- The rate bar and the tiles must match your sources.
- With a source above 2.3 MHz (Release 1's old limit), the rate must follow it
  up to 11.9 M/s per channel.
- The log must show no `TDC CHIP READOUT LIMIT` or `EVENTS LOST` banners
  while a single channel stays well under the limit.

**B5. Timing, as before.** Repeat one of your Release 1 histograms. For the
1 kHz square wave on STOP1 + STOP2, use a **Histogram** with click 2, start
1, 100 ns bins × 200 (STOP2 came 6.3 µs after STOP1 last time). The peak must
sit where Release 1 put it. With 10 ps bins around it, expect the GPX2's
~44 ps comb (the chip's, not a software effect); the Gaussian fit's
*envelope of peaks* gives its width.

**B6. Live trigger change.** While B4/B5 run, change a trigger in a panel's
Channels table. The log should say `trigger N = ...`, the measurements keep
running, and the rate follows the new trigger.

**B7. Record and replay.** Use **+ New measurement → TimeTagStream**: stream for
2 s. Then **open in a new Histogram** with the same settings as B5: same peak.

**B8. A long silence** (checks the 24-bit index at a low rate). With only a
slow source connected (or the triggers above the signal for a while), run a
Correlation or Histogram for a minute with gaps of seconds between events.
Peaks must stay sharp. Close the app.

## C. Python

```sh
cd /tmp
PY=/home/qittlab/dws/SDE/.venv/bin/python
PYTHONPATH=/home/qittlab/dws/Dantagger/python $PY -c "
import Dantagger as T
t = T.createTimeTagger(); print(t._t.info())
s = T.SynchronizedMeasurements(t); c = T.Countrate(s.getTagger(), [1, 2, 3, 4])
s.startFor(int(1e12)); s.waitUntilFinished(); print(c.getData())
T.freeTimeTagger(t)"
```

- The first line must be `board already configured and calibrated (phase step
  P): setup skipped` (the app set it up).
- `info()` must show `24 + 18-bit frames ... 11.90 M events/s` and
  `re-anchors 0`.
- The rates must match the app's.
- Opening does not change your triggers.

Run it twice; both say "setup skipped". The SDE scripts still import
`SDE/Dantagger` through the switch: tell me if you want it pointed here
before running them.

## D. Fallback (only if A3 or A4 fails)

Do the fallback in the **app**: its calibration is recorded, and from then on
*Set up the board* and Python use that rate and data rate, also after power
cycles. tdcctl calibrations are not recorded.

On the *TDC-GPX2 & LVDS* page:

1. *GPX2 configuration (manual)*: 24 ref bits, 18 stop bits (the defaults);
   **DDR** on or off per the rung.
2. *LVDS link*: pick the **LCLKIN** rate.
3. **Calibrate**. Then check the State line and run B4.

| Rung | LCLKIN | DDR | Per-channel limit (42-bit frames) |
|---|---|---|---|
| 1 | 250 MHz | off (SDR) | 5.95 M/s |
| 2 | 125 MHz | on | 5.95 M/s |
| 3 | 125 MHz | off | 2.98 M/s |
| 4 | 62.5 MHz | on | 2.98 M/s (just above the 2.73 MHz SDE laser) |

The same rungs with tdcctl, for the calibration tables:

```sh
$T gpx-config --sdr $F; $T calibrate --sdr $F                                # 250 MHz SDR
$T lvds off --div 1; $T gpx-config $F; $T calibrate --div 1 $F               # 125 MHz DDR
$T lvds off --div 1; $T gpx-config --sdr $F; $T calibrate --div 1 --sdr $F   # 125 MHz SDR
$T lvds off --div 2; $T gpx-config $F; $T calibrate --div 2 $F               # 62.5 MHz DDR
```

If even 62.5 MHz DDR fails, stop and send the calibration tables: that would
point at something other than the bit rate. To go back to 250 MHz DDR later,
calibrate there once in the app.

## E. Back to Release 1

Power-cycle: Release 1 boots from flash. Dantagger keeps the two releases'
calibrations apart, so Release 1 gets its own setup (24 + 20 bits SDR,
2.27 M events/s per channel), and Release 2's (including a fallback) is still
there the next time it is loaded. Release 1 is too slow for a 2.73 MHz start
channel; see AGENTS.md (open items).
