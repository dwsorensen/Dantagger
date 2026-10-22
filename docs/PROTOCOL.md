# TDC firmware protocol (release 2, design version 0x54440002)

This is the host ↔ FPGA contract for `2_tdc`. Release 1 (`1_tdc`, design
0x54440001) is frozen; its contract is this file at git tag `release-1`
(`releases/release-1/`). Release 2 changes only the LVDS readout and what
describes it: LCLKIN up to 250 MHz, DDR, run-time sampling of every lane at
500 MS/s, 8 ns coarse time, three new response fields (§2.1, §2.3, §3.3, §4
mark the differences). The host tools speak both and tell them apart by field 1
of any response.

It covers both modes:

- **Configuration mode** (the default): comparator control (as in
  `../Comparator_Control/PROTOCOL.md`), TDC-GPX2 configuration with readback,
  TMP117 temperature, and the LVDS link controls and diagnostics.
- **Streaming mode**: time-tagged GPX2 events from the four STOP channels,
  sent to the host in DATA frames.

## 1. Envelope and link

The envelope and link rules are Test_project protocol v1 unchanged
(`../Test_project/PROTOCOL_AND_TESTS.md` §1, §8 and §10):

- fixed 4096-byte frames, magic `AU2FT601`, version 1, little-endian;
- CRC-32/ISO-HDLC trailer over bytes 0–4091;
- the FPGA silently drops (and counts as discarded) any COMMAND frame with:
  - a bad magic, version or type, or a length other than 4096;
  - a nonzero offset/seed/flags/timestamp header word;
  - BE ≠ 1111 on any word, or a CRC error;
  - a partial frame idle for 10 ms;
- one command outstanding; every accepted frame gets exactly one RESPONSE;
- the release-10 arbitration: no frame takes the bus while TXE_N is high,
  and at least 16 released FT cycles follow every DATA frame (128 after every
  8th DATA frame), so a waiting command is read at a frame boundary even under
  back-pressure, including by an FT601 that presents it only after WR_N has
  been high for up to 128 cycles (the hypothesis behind Test_project's
  release-9 STOP finding).

The FT601 must be in its recorded configuration: 245 mode, 1 channel,
66.67 MHz. The FPGA drives DATA/BE only to answer a CRC-valid COMMAND or to
send the DATA of a run that a CRC-valid START began.

Words are numbered from the start of the frame (word 0 = bytes 0–3).

## 2. Commands

Header word 10 is the opcode; word 12 is the valid payload length in bytes.
Payload words (u32) start at word 16. Every payload word at or after the
valid length must be zero, otherwise BAD_ARGUMENT. Run ID (words 4–5) and
transaction ID (words 6–7) are echoed in the response.

| Opcode | Name | Length | Payload (u32 words) | Allowed while streaming |
|---|---|---|---|---|
| 1 | START | 8 | window µs (0 = until STOP), flags (must be 0) | no (BUSY) |
| 2 | STOP | 0 | – | yes |
| 3 | STATUS | 0 | – | yes |
| 6 | SET_LEDS | 4 | Au LED mask ≤ 0xFF | no |
| 0x20 | SET_THRESHOLDS | 20 | channel mask, code ch1..ch4 | no |
| 0x21 | SET_HYSTERESIS | 20 | channel mask, wiper ch1..ch4 | no |
| 0x22 | SET_LED1 | 16 | red, green, blue, current | no |
| 0x23 | SET_LED2 | 4 | on-mask | no |
| 0x24 | PROBE | 0 | – | no |
| 0x25 | READBACK | 0 | – | no |
| 0x30 | GPX_CONFIGURE | 20 | GPX2 registers 0–16, packed (below) | no |
| 0x31 | GPX_READ_CONFIG | 0 | – | no |
| 0x32 | GPX_RESET | 0 | – | no |
| 0x33 | GPX_INIT | 0 | – | no |
| 0x34 | GPX_READ_RESULTS | 0 | – | no |
| 0x40 | LVDS_SET | 20 | enable, divider code, bit phase, SDO taps, FRAME taps | no |
| 0x41 | LVDS_PHASE | 4 | target phase step | no |
| 0x42 | LINK_TEST | 4 | window in lclk_rx cycles (8 ns) | no |
| other | – | – | – | BAD_COMMAND |

The comparator commands (0x20–0x25) and SET_LEDS are exactly those of
Comparator_Control release 1, including `DAC_CODE_MAX` = 41953 and the
argument rules (`../Comparator_Control/PROTOCOL.md`).

**Response status** (header word 13). When several apply, the first in this
list wins: BAD_COMMAND, BAD_ARGUMENT, BUSY, NOT_READY, DEVICE_ERROR.

| Code | Name | Meaning |
|---|---|---|
| 0 | OK | – |
| 1 | BAD_COMMAND | unknown opcode |
| 2 | BAD_ARGUMENT | bad length, value or nonzero padding; nothing is done |
| 3 | BUSY | the command is not allowed while streaming; nothing is done |
| 5 | DEVICE_ERROR | the operation ran, but a device NACKed, an ID or readback did not match, or a bus timed out |
| 6 | NOT_READY | START: the GPX2 is not measuring with a verified configuration, or LCLKIN is off, or the LVDS clocks are not locked. GPX_INIT: the configuration is not verified. Nothing is done. |

### 2.1 GPX_CONFIGURE (0x30)

Payload: the 17 configuration bytes, register r in byte r of the payload
(word 16 + r/4, bits 8·(r mod 4) and up). Word 20 bits 31:8 must be zero.

The FPGA rejects the command (BAD_ARGUMENT, nothing is sent to the GPX2)
unless every board rule and every fixed bit holds:

| Register | Rule | Reason |
|---|---|---|
| 0 | bit 4 `PIN_ENA_REFCLK` = 1 | REFCLK comes from the LMK1D1204 |
| 1 | bits 7:6 `HIGH_RESOLUTION` ≠ 3; bits 5:4 `CHANNEL_COMBINE` ≠ 3 | undefined codes |
| 3–5 | `REFCLK_DIVISIONS` ≥ 1 and ≤ 2^stop_bits − 1 | datasheet rule for LVDS output |
| 5 | bits 7:4 = 0000 | fixed (ams) |
| 6 | bits 7:5 = 110, bits 3:0 = 0000 | fixed (ams); bit 4 `LVDS_TEST_PATTERN` is free |
| 7 | bit 7 `REFCLK_BY_XOSC` = 0; bit 6 = 1; bits 3:0 = 0011 | board rule; fixed (ams) |
| 8–15 | = A1 13 00 0A CC CC F1 7D (hex) | fixed (ams) |
| 16 | = 00 | `CMOS_INPUT` = 0 (every input is LVDS); fixed bits 0 |

`stop_bits` is 14, 16, 18 or 20 from register 2 bits 4:3. Release 2 accepts
`LVDS_DOUBLE_DATA_RATE` (register 2 bit 5) = 1 (release 1 refused it).

The operation (GPX2 SPI, mode 1, 1 MHz, every access starts with an SSN
high pulse):

1. `spiopc_power` (0x30), then wait 1 ms. The GPX2 stops and resets its
   configuration.
2. `spiopc_write_config` + 0 (0x80), then registers 0–16.
3. `spiopc_read_config` + 0 (0x40), read 17 bytes, compare.
4. Only if all 17 match: `spiopc_init` (0x18), wait 1 ms, then read the
   configuration again and compare (INIT must preserve it).
5. Sample PARITY and INTERRUPT.

Status is OK only if both readbacks matched; otherwise DEVICE_ERROR. The
GPX2 state flags (field 38) record the outcome. A successful CONFIGURE also
sets the capture format (field 61) from register 2: the widths and SDR/DDR.

### 2.2 Other GPX2 commands

| Command | SPI | Notes |
|---|---|---|
| GPX_READ_CONFIG | 0x40, 17 bytes | compares with the last written configuration (mismatch mask, field 39); DEVICE_ERROR on a mismatch; does not change the state flags |
| GPX_RESET | 0x30, wait 1 ms | clears configured / verified / measuring and the capture format |
| GPX_INIT | 0x18, wait 1 ms | NOT_READY unless the configuration is verified; restarts the measurement and resets the reference index |
| GPX_READ_RESULTS | 0x68 (`spiopc_read_results` + 8), 24 bytes | result registers 8–31 into fields 50–55. The datasheet requires LVDS readout to be off (PIN_ENA_LVDS_OUT = 0 or no LCLKIN) for SPI readout; the FPGA does not enforce this. An empty channel reads 0xFFFFFF. |

PARITY (field 38 bit 3) is "the sum of all configuration bits is even (0)
or odd (1)". The FPGA also reports the parity it computes from the last
readback (bit 4). It does not fail an operation on a parity difference.

### 2.3 LVDS link controls

The FPGA generates LCLKIN itself and samples SDO1–4 and FRAME1–4 with its
own clock. It never clocks data with LCLKOUT (that pin is in another bank);
it only samples it for diagnostics. The sampling point is set by the host.

Clocking (build constants, response fields 108–110; the 250 MHz build):
F = 250 MHz. LCLKIN = F / 2^code. Every lane (and LCLKOUT) is sampled on both
edges of an F clock (ISERDESE2), i.e. every 2 ns, four samples per lclk_rx
cycle (F/2 = 125 MHz, 8 ns). The GPX2 sends one bit per LCLK period (SDR) or
two (DDR, register 2 bit 5), so a bit lasts N = 2^(code + (DDR ? 0 : 1))
samples: 1 (250 MHz DDR) … 16 (31.25 MHz SDR).

- **LVDS_SET (0x40)**, payload:
  - word 0: LCLKIN enable, 0 or 1. LCLKIN is tri-stated (both legs high
    impedance) from configuration until enabled, and again when disabled.
  - word 1: divider code 0–3: LCLKIN = 250, 125, 62.5 or 31.25 MHz.
  - word 2: bit phase 0–15: the deserializer keeps the samples whose index
    (counted from the LVDS clock lock) is ≡ bit phase (mod N); values ≥ N
    are taken mod N.
  - word 3: IDELAY taps for SDO1..SDO4, one byte each (byte c = channel c+1), 0–31.
  - word 4: IDELAY taps for FRAME1..FRAME4, likewise.

  Each tap is 78 ps (IDELAYE2, 200 MHz reference). Applying settings
  restarts the deserializers. The sample count and the LCLKIN divider both
  run freely from the LVDS clock lock, so a bit phase keeps its meaning
  across LVDS_SET (LCLKIN off and on, other dividers) as long as the phase
  (below) is the same. Every LCLKIN level lasts at least 2 ns: LCLKIN never
  exceeds 250 MHz, also while the divider changes.

- **LVDS_PHASE (0x41)**, payload: the target phase step 0–223. The
  sampling clocks' phase is moved (MMCM fine phase shift, up or down, never
  wrapping) until it is that many steps of 1/56 VCO period (17.857 ps)
  behind the lock position; 224 steps are one 4 ns period of F (two
  samples). The current step is in field 58. The sampling instant within a
  bit is (bit phase mod N) × 2 ns + phase × 17.857 ps, so phases 0–111 with
  every bit phase cover one bit; the host calibrates that way.

- **LINK_TEST (0x42)**, payload: a window of 1 … 100,000,000 lclk_rx cycles
  (8 ns). The FPGA clears the link counters, counts for the window, and
  returns them (fields 62–90). It does not change the GPX2 configuration:
  to test with the GPX2 test pattern, configure with `LVDS_TEST_PATTERN` = 1
  first. Counters per channel c (lanes SDO c+1 and FRAME c+1):
  - frames: complete frames decoded (a frame starts where FRAME rises);
  - good: frames whose FRAME pattern is exactly 8 high bits then low, and
    whose reference index and stop result equal the datasheet test pattern
    truncated to the configured widths (reference index 0xF0CCAA, stop result
    0x0AACF0);
  - bad pattern: frames with a correct FRAME pattern but other data;
  - structure errors: FRAME bits that broke the pattern (a frame is
    abandoned there; a FRAME rise at that bit starts a new frame);
  - SDO ones, FRAME ones: decoded bits that read 1 (shows a lane is alive
    or stuck).

  The LCLKOUT histogram counts, for each of 8 slots (slot = sample index
  mod 8, every sample of LCLKOUT, 2 ns apart), how many samples read 1. At
  250 MHz LCLKOUT the even and odd slots show its two halves.

Frames are decoded only while a capture format is valid (after a successful
GPX_CONFIGURE).

## 3. Streaming

### 3.1 Run control

- **START** (configuration mode only; BUSY while streaming). Rejected
  with NOT_READY unless the GPX2 is measuring with a verified configuration,
  LCLKIN is enabled, and the LVDS clocks and IDELAYCTRL are ready. On OK:
  - the run ID is the header's; the sequence and run counters reset;
  - the START response is sent before any DATA frame of the run;
  - the FPGA discards any stale records left in its event FIFO, then
    enables capture;
  - window (word 0): capture stops by itself after this many microseconds,
    0 = until STOP.
- **STOP**: when not streaming, OK at once. While streaming, capture stops
  at once; every record already captured is still sent; the STOP response
  follows the last DATA frame of the run.
- **Window end**: when the window elapses, capture stops; after the last
  DATA frame the FPGA sends an unsolicited RESPONSE with opcode
  RUN_COMPLETE = 0x101, the START's transaction ID and run ID — unless a STOP
  arrived first, in which case the STOP response is sent instead (exactly one
  final response per run).
- While streaming, only STATUS and STOP are accepted; every other command
  is answered BUSY and does nothing. A second STOP while one is pending is
  answered BUSY.
- If the FT link is lost, the run ends (no final response). Records left in
  the FIFO are discarded by the next START.

### 3.2 DATA frames

| Word | Content |
|---|---|
| 2 | 0x00010001 (version 1, DATA) |
| 3 | 4096 |
| 4–5 | run ID |
| 6–7 | sequence (0-based, per run) |
| 8–9 | records sent in this run before this frame |
| 10 | record format = 1 |
| 11 | 0 |
| 12 | payload bytes = 8 × N, N = 1 … 503 records |
| 13 | 0 |
| 14–15 | FT-clock timestamp when the frame was built |
| 16 … 16+2N−1 | N records, each low word then high word |
| … 1022 | 0 |
| 1023 | CRC |

A frame is built when 503 records are waiting, or when records have waited
1 ms, or at once while the run is ending.

### 3.3 Records (64 bits)

Bit 63 selects the kind.

**Event** (bit 63 = 0): one GPX2 result.

| Bits | Field |
|---|---|
| 62:61 | channel 0–3 (STOP1–STOP4; STOP3 is inverted on purpose at the comparator) |
| 60 | 0 |
| 59:44 | coarse time, low 16 bits (FPGA 100 MHz cycles, 10 ns) |
| 43:20 | reference index (24 bits; zero above the configured width) |
| 19:0 | stop result (20 bits; zero above the configured width) |

**Marker** (bit 63 = 1):

| Bits | Field |
|---|---|
| 62:60 | type: 0 START, 1 TIME, 2 OVERFLOW, 3 END |
| 59:48 | OVERFLOW: events dropped since the previous record, saturating at 4095. END: bit 48 = 1 if the window ended the run, 0 for STOP. Otherwise 0. |
| 47:0 | coarse time, all 48 bits |

- The first record of a run is START; the last is END.
- The coarse time counts lclk_rx cycles: 8 ns in the 250 MHz build (field
  109; release 1: 10 ns).
- A TIME marker is written whenever the low 14 bits of the coarse time wrap
  (every 131.072 µs), so consecutive records are never more than 2^15 cycles
  apart: the host unwraps each event's 16-bit coarse time against the
  previous record (signed difference of the low 16 bits).
- An event that cannot enter the event FIFO within 8 cycles of being
  received (the FIFO is full, or its channel's previous event is still
  waiting) is dropped and counted; as soon as there is space, an OVERFLOW
  marker records the count. Markers wait for space; they are never dropped,
  and they go before waiting events, so the first record after the FIFO has
  been full carries the full coarse time. No record is more than 8 cycles
  older than the record before it.
- The coarse time of an event is taken when its LVDS frame has been
  received, so it trails the stop edge by the GPX2 latency and readout time
  (up to 16 frames when the GPX2 FIFO is full, 0.8 µs for 24-bit frames at
  250 MHz DDR). It serves to unwrap the reference index, not to time events.
  The reference index must wrap slowly enough for that: the host requires
  2^ref_bits REFCLK periods ≥ 4 × (17 frames + 0.5 µs) (6 bits at 250 MHz DDR
  with 18 stop bits; 8 bits at 125 MHz DDR or 250 MHz SDR).
- Time of an event, in units of the GPX2 LSB: reference index ×
  REFCLK_DIVISIONS + stop result, after unwrapping the reference index with
  the coarse time (the host knows the REFCLK frequency; the FPGA does not).

## 4. RESPONSE payload

Valid length 444 bytes: 111 little-endian u32 fields at words 16–126.
Words 127–1022 are zero. (Release 1: 436 bytes, 109 fields.)

| Field | Content |
|---|---|
| 0 | opcode |
| 1 | design version 0x54440002 (release 1: 0x54440001) |
| 2 | fault bits: bit 0 FT link lost since FPGA reset; bit 1 a partial command timed out; bit 2 event FIFO read underrun (never expected) |
| 3 | valid command frames since the link came up |
| 4 | discarded command frames since the link came up |
| 5 | Au LED mask |
| 6 | flags: bit 0 board boot done; 1 DAC OK; 2 MCP23S08 OK; 3 U3 OK; 4 U8 OK; 5 AW2013 OK; 6 TMP117 OK; 7 LED mask set |
| 7 | comparator board first error (Comparator_Control format) |
| 8 | board error count |
| 9 | board operation count |
| 10–13 | DAC codes ch1–ch4 (read back) |
| 14–17 | DAC DEVICEID, GAIN, CONFIG, STATUS |
| 18–21 | wipers ch1–ch4 (raw; value in bits 8:0) |
| 22–24 | AW2013 ID, GCR, LCTR |
| 25–27 | AW2013 LCFG0–2 |
| 28–30 | AW2013 PWM0–2 |
| 31–32 | MCP23S08 IODIR, OLAT |
| 33 | DAC code limit, 41953 |
| 34 | TMP117 Device_ID register (expect 0x0117) |
| 35 | TMP117 Temp_Result (16-bit two's complement, 7.8125 m°C per LSB) |
| 36 | TMP117 Configuration register |
| 37 | TMP117 polls: bits 15:0 successful (wrap), bits 31:16 failed (saturate) |
| 38 | GPX2 flags: bit 0 configured; 1 verified; 2 measuring; 3 PARITY pin; 4 parity of readback; 5 INTERRUPT pin (raw, active low); 6 last GPX2 operation failed; 7 test pattern on in the written configuration |
| 39 | GPX2 mismatch mask (bit r = register r differed in the last readback) |
| 40–44 | written configuration, registers 0–16 packed like the CONFIGURE payload |
| 45–49 | last readback, likewise |
| 50–55 | result registers 8–31 from the last GPX_READ_RESULTS, byte (r−8) of the packed bytes |
| 56 | GPX2 operations: bits 15:0 count, bits 31:16 failed |
| 57 | GPX2 last error: bits 7:0 kind (1 readback mismatch, 2 post-INIT readback mismatch, 3 INIT refused), bits 15:8 GPX2 operation (1 CONFIGURE, 2 READ_CONFIG, 3 RESET, 4 INIT, 5 READ_RESULTS) |
| 58 | LVDS: bit 0 LCLKIN enabled; bits 2:1 divider code; bits 6:3 bit phase; bit 8 LVDS MMCM locked; bit 9 IDELAYCTRL ready; bit 10 LVDS controller timeout seen; bits 31:16 phase step |
| 59 | SDO taps (byte c = channel c+1) |
| 60 | FRAME taps |
| 61 | capture format: bits 7:0 reference-index bits; 15:8 stop bits; bit 16 valid; bit 17 DDR |
| 62 | window of the last LINK_TEST (lclk_rx cycles) |
| 63–86 | per channel c, at 63 + 6c: frames, good, bad pattern, structure errors, SDO ones, FRAME ones |
| 87–90 | LCLKOUT histogram: slot 2i in bits 15:0 and slot 2i+1 in bits 31:16 of field 87+i (16-bit, saturating; slot = sample index mod 8) |
| 91 | stream: bit 0 capture requested; bit 1 draining (run ending); bit 2 capture active (LVDS side); bit 3 window ended; bit 4 STOP pending |
| 92 | window µs of the current or last run |
| 93–94 | records sent this run (u64) |
| 95–96 | DATA frames sent this run (u64) |
| 97 | events dropped this run (FIFO full; saturating) |
| 98–99 | events captured this run (written to the FIFO; u64) |
| 100 | cycles with structure errors while capturing this run (those frames are not delivered; saturating) |
| 101 | event FIFO highest fill this run (records) |
| 102–103 | TXE stall cycles this run (u64) |
| 104–105 | FT cycles this run with RXF_N low while the FPGA held the bus to transmit (u64) |
| 106–107 | run ID of the current or last run |
| 108 | LCLKIN at divider code 0, kHz (250000) |
| 109 | lclk_rx period = coarse tick = LINK_TEST cycle, ps (8000) |
| 110 | bits 15:0 LVDS_PHASE steps (224); bits 23:16 samples per lclk_rx cycle (4) |

Fields 56–107 that come from the LVDS side (58 bits 8–10, 62–90, 91 bits
2–3, 97–101) are a snapshot taken during the command; the others are exact
at the response.

## 5. LEDs

Once SET_LEDS has been accepted and the link is up, the LEDs show the host
pattern. Otherwise: LED0 heartbeat; LED1 FT link up; LED2 board boot done;
LED3 GPX2 verified; LED4 LCLKIN enabled; LED5 streaming; LED6 TMP117 OK;
LED7 FT link lost, a board error or a failed GPX2 operation since reset.
