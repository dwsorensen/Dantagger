"""Dantagger: a drop-in replacement for the parts of the Swabian ``TimeTagger``
library that the SDE measurement scripts use, running on the TDC board
(TDC_Firmware: Alchitry Au V2 + FT601 + comparator board + TDC-GPX2).

    import Dantagger as TimeTagger
    tagger = TimeTagger.createTimeTagger()          # configures + calibrates on the first open after plug-in
    tagger.setTriggerLevel(1, 0.05)
    sync = TimeTagger.SynchronizedMeasurements(tagger)
    cr = TimeTagger.Countrate(sync.getTagger(), [1])
    sync.startFor(int(1e12)); sync.waitUntilFinished()
    print(cr.getData())
    TimeTagger.freeTimeTagger(tagger)

Differences from Swabian (see README.md): channels are 1-4; negative
(falling-edge) channel numbers are mapped to the positive channel with a
warning; trigger levels must be within -0.100 .. +1.500 V; settings cannot
change while a started measurement runs; the board streams only while a
measurement runs; overflows are announced with banners that cannot be missed.
Dantagger-only: ``setHysteresis``, ``setHighResolution``,
``createSimulatedTagger``, ``createReplayTagger``.
"""

import atexit
import enum
import json
import math
import os
import sys
import threading
import time
import weakref

import numpy as np

from . import _dantagger as _core
from ._dantagger import DeviceError, SettingsBusyError

__all__ = [
    "createTimeTagger", "createSimulatedTagger", "createReplayTagger", "freeTimeTagger", "scanTimeTagger",
    "TimeTagger", "SynchronizedMeasurements", "Countrate", "Counter", "Correlation", "Histogram", "HistogramND",
    "TimeTagStream", "TimeTagStreamBuffer", "Resolution", "ChannelEdge", "CHANNEL_UNUSED", "setHysteresis",
    "setHighResolution", "DeviceError", "SettingsBusyError", "setLogger", "getVersion",
]

__version__ = "0.1.0"
CHANNEL_UNUSED = -134217728
CHANNEL_UNUSED_OLD = -1   # name exists in old Swabian releases; never treated as a channel here
NUM_CHANNELS = 4

# Swabian logger constants (setLogger is accepted; Dantagger prints its own warnings).
LOGGER_ERROR, LOGGER_WARNING, LOGGER_INFO = 40, 30, 10
# Swabian test-signal / level names some scripts import.
UNKNOWN, LOW, HIGH = 0, 1, 2


class Resolution(enum.IntEnum):
    """Accepted for compatibility; Dantagger's resolution is set with
    ``tagger.setHighResolution(1|2|4)`` (TDC-GPX2 HIGH_RESOLUTION)."""
    Standard = 0
    HighResA = 1
    HighResB = 2
    HighResC = 3


class ChannelEdge(enum.IntFlag):
    All = 0
    Rising = 1
    Falling = 2
    HighResAll = 4
    HighResRising = 8
    HighResFalling = 16
    StandardAll = 32
    StandardRising = 64
    StandardFalling = 128


class TagType(enum.IntEnum):
    TimeTag = 0
    Error = 1
    OverflowBegin = 2
    OverflowEnd = 3
    MissedEvents = 4


def getVersion():
    return f"Dantagger {__version__}"


_logger = None


def setLogger(callback):
    """Accepted for compatibility. Dantagger's warnings also go to `callback(level, message)` if given."""
    global _logger
    _logger = callback


# ---------------------------------------------------------------------------
# Warnings that cannot be missed
# ---------------------------------------------------------------------------
_print_lock = threading.Lock()


def _banner(title, text, char="#"):
    width = 100
    color = sys.stderr.isatty()
    red, reset = ("\033[1;31m", "\033[0m") if color else ("", "")
    lines = [char * width, f"{char*3}  {title}  ".ljust(width - 3) + char * 3]
    words, line = text.split(), ""
    for w in words:
        if len(line) + len(w) + 1 > width - 10:
            lines.append(f"{char*3}  {line}".ljust(width - 3) + char * 3)
            line = w
        else:
            line = f"{line} {w}" if line else w
    if line:
        lines.append(f"{char*3}  {line}".ljust(width - 3) + char * 3)
    lines.append(char * width)
    with _print_lock:
        sys.stderr.write(red + "\n".join(lines) + reset + "\n")
        sys.stderr.flush()
    if _logger is not None:
        try:
            _logger(LOGGER_WARNING, f"{title}: {text}")
        except Exception:
            pass


def _note(text):
    with _print_lock:
        sys.stderr.write(f"[Dantagger] {text}\n")
        sys.stderr.flush()


_TITLES = {
    "overflow": "DANTAGGER OVERFLOW !!! EVENTS LOST !!! DANTAGGER OVERFLOW !!! EVENTS LOST",
    "saturation": "DANTAGGER: TDC CHIP READOUT LIMIT !!! EVENTS PROBABLY LOST INSIDE THE GPX2",
    "device": "DANTAGGER DEVICE FAILURE",
    "stream_full": "DANTAGGER: TimeTagStream BUFFER FULL, TAGS DISCARDED",
}

_open_taggers = weakref.WeakSet()


def _pump(tagger):
    try:
        items = tagger._t.take_warnings()
    except Exception:
        return
    for kind, text in items:
        if kind in _TITLES:
            _banner(_TITLES[kind], text, "!" if kind in ("overflow", "saturation", "device") else "#")
        else:
            _note(text)


def _pump_all():
    for t in list(_open_taggers):
        _pump(t)


def _pump_thread():
    while True:
        time.sleep(0.25)
        try:
            _pump_all()
        except Exception:
            pass


threading.Thread(target=_pump_thread, name="dantagger-warnings", daemon=True).start()


def _summary(tagger):
    t = tagger._t
    dropped = t.dropped_total()
    sat = t.warning_count("saturation")
    if dropped or sat:
        parts = []
        if dropped:
            parts.append(f"{dropped} EVENTS WERE LOST IN {t.overflow_intervals()} FPGA OVERFLOW INTERVAL(S)")
        if sat:
            parts.append(f"{sat} TDC-CHIP READOUT-LIMIT WARNING(S)")
        _banner("DANTAGGER SESSION SUMMARY: DATA WAS LOST",
                "; ".join(parts) + ". Count rates and histograms from the affected intervals are incomplete. "
                "See the OVERFLOW banners above for the times.", "!")


@atexit.register
def _close_all():
    for t in list(_open_taggers):
        try:
            freeTimeTagger(t)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Argument helpers
# ---------------------------------------------------------------------------
def _ps(value, what="time"):
    """Swabian takes integer picoseconds; scripts often pass floats (t*1e12)."""
    try:
        v = float(value)
    except (TypeError, ValueError):
        raise TypeError(f"{what} must be a number of picoseconds, not {value!r}") from None
    if not math.isfinite(v):
        raise ValueError(f"{what} must be finite")
    return int(round(v))


def _int(value, what):
    try:
        v = float(value)
    except (TypeError, ValueError):
        raise TypeError(f"{what} must be a number, not {value!r}") from None
    if not math.isfinite(v) or v != round(v):
        raise ValueError(f"{what} must be an integer, not {value!r}")
    return int(v)


_falling_warned = {}


def _channel(ch, what="channel"):
    """Dantagger channels are 1..4.  Negative (falling-edge) channels map to
    the positive channel with a warning; the board has no falling-edge mode."""
    c = _int(ch, what)
    if c == CHANNEL_UNUSED:
        raise ValueError(f"{what}: CHANNEL_UNUSED is not a channel here")
    if c < 0:
        now = time.monotonic()
        last = _falling_warned.get(c)
        if last is None:
            _banner(f"FALLING EDGES NOT SUPPORTED YET, SETTING CHANNEL {-c}",
                    f"{what} {c} asks for the falling edge of input {-c}. The TDC board time-tags one edge per "
                    f"input, so Dantagger uses channel {-c} (its normal edge) instead. Note: input 3 is wired "
                    f"inverted on the board, so channel 3 is the opposite edge of input 3.", "!")
            _falling_warned[c] = now
        elif now - last > 30:
            _note(f"FALLING EDGES NOT SUPPORTED YET: channel {c} used as channel {-c}")
            _falling_warned[c] = now
        c = -c
    if not 1 <= c <= NUM_CHANNELS:
        raise ValueError(f"{what} {ch} does not exist: the TDC board has channels 1-{NUM_CHANNELS}")
    return c


def _channels(chs, what="channel"):
    if isinstance(chs, (int, np.integer)):
        chs = [chs]
    return [_channel(c, what) for c in chs]


# ---------------------------------------------------------------------------
# The time tagger
# ---------------------------------------------------------------------------
class TimeTagger:
    """The board. Create with createTimeTagger() (or the simulated/replay factories)."""

    def __init__(self, core):
        self._t = core
        self._sw_delay = {c: 0 for c in range(1, NUM_CHANNELS + 1)}
        self._hw_delay = {c: 0 for c in range(1, NUM_CHANNELS + 1)}
        _open_taggers.add(self)

    # -- input settings ------------------------------------------------------
    def setTriggerLevel(self, channel, voltage):
        """Comparator threshold in volts; must be within -0.100 .. +1.500 V (board safety window)."""
        self._t.set_trigger_level(_channel(channel), float(voltage))

    def getTriggerLevel(self, channel):
        return self._t.trigger_level(_channel(channel))

    def setDeadtime(self, channel, deadtime):
        """Software dead time in ps (the GPX2 itself cannot resolve pulses closer than 20 ns).
        Returns the effective dead time."""
        return int(self._t.set_deadtime(_channel(channel), _ps(deadtime, "deadtime")))

    def getDeadtime(self, channel):
        return int(self._t.deadtime(_channel(channel)))

    def _apply_delay(self, c):
        self._t.set_delay(c, self._sw_delay[c] + self._hw_delay[c])

    def setInputDelay(self, channel, delay):
        """Input delay in ps (applied in software, exactly; any sign)."""
        c = _channel(channel)
        self._sw_delay[c], self._hw_delay[c] = _ps(delay, "delay"), 0
        self._apply_delay(c)

    def getInputDelay(self, channel):
        c = _channel(channel)
        return self._sw_delay[c] + self._hw_delay[c]

    def setDelaySoftware(self, channel, delay):
        c = _channel(channel)
        self._sw_delay[c] = _ps(delay, "delay")
        self._apply_delay(c)

    def getDelaySoftware(self, channel):
        return self._sw_delay[_channel(channel)]

    def setDelayHardware(self, channel, delay):
        """The board has no hardware delay; this adds to the software delay."""
        c = _channel(channel)
        self._hw_delay[c] = _ps(delay, "delay")
        self._apply_delay(c)

    def getDelayHardware(self, channel):
        return self._hw_delay[_channel(channel)]

    # -- Dantagger-only ------------------------------------------------------
    def setHysteresis(self, channel, value):
        """Comparator hysteresis setting (TLV3901 HYS resistance) in ohms. LOWER resistance = MORE hysteresis
        (~0 ohm: 47 mV, 800 ohm: 17 mV, >= 4 kohm: 1 mV). Values outside 75 .. 5075 ohm are clamped.
        Returns the applied resistance."""
        v = float(value)
        applied = self._t.set_hysteresis(_channel(channel), v)
        if abs(applied - v) > 25:
            _note(f"hysteresis {v:g} ohm on channel {_channel(channel)} applied as {applied:.0f} ohm "
                  f"(hardware range 75 .. 5075 ohm, 256 steps)")
        return applied

    def getHysteresis(self, channel):
        return self._t.hysteresis(_channel(channel))

    def setHighResolution(self, factor):
        """TDC-GPX2 HIGH_RESOLUTION: 1 = off (default, ~20 ps rms, 20 ns pulse spacing), 2 = 2x (~15 ps rms,
        50 ns), 4 = 4x (~10 ps rms, 100 ns). Reconfigures the chip (no recalibration)."""
        self._t.set_resolution(_int(factor, "resolution factor"))

    def getHighResolution(self):
        return self._t.resolution()

    def reset(self):
        """Every setting back to its default (trigger 0.010 V, hysteresis 1000 ohm, no delay/deadtime, standard
        resolution)."""
        self._t.reset()
        for c in self._sw_delay:
            self._sw_delay[c] = self._hw_delay[c] = 0

    # -- information ------------------------------------------------------------
    def getSerial(self):
        return self._t.serial()

    def getModel(self):
        return self._t.model()

    def getChannelList(self, *args, **kwargs):
        return list(range(1, NUM_CHANNELS + 1))

    def info(self):
        return self._t.info()

    def getConfiguration(self):
        return json.dumps({
            "serial": self.getSerial(), "model": self.getModel(),
            "channels": {c: {"trigger_level": self._t.trigger_level(c), "hysteresis_ohm": self._t.hysteresis(c),
                             "deadtime": self._t.deadtime(c), "delay": self.getInputDelay(c)}
                         for c in range(1, NUM_CHANNELS + 1)},
            "high_resolution": self._t.resolution(),
        })

    def getOverflows(self):
        """Dantagger: events lost in FPGA overflows since the board was opened."""
        return int(self._t.dropped_total())

    def __repr__(self):
        return f"<Dantagger.TimeTagger {self.getSerial()}>"


def _resolve_simulate(simulate):
    if simulate is None:
        simulate = os.environ.get("DANTAGGER_SIMULATE", "").strip().lower() or False
    if simulate in (False, "0", "no", "false", ""):
        return None
    if simulate in (True, "1", "yes", "true", "sim", "signals"):
        return "signals"
    if simulate == "fpga":
        return "fpga"
    raise ValueError(f"unknown simulate mode {simulate!r} (use True/'signals' or 'fpga')")


def _frame():
    env = os.environ.get("DANTAGGER_FRAME")
    if env:
        a, b = (int(x) for x in env.split(","))
        return a, b
    return 0, 0   # the frame policy of the loaded firmware release


def createTimeTagger(serial="", resolution=Resolution.Standard, *, simulate=None, recalibrate=False,
                     frame=None, state_dir=""):
    """Opens the TDC board.  The first open after the board is plugged in
    configures the TDC-GPX2 and calibrates the LVDS link (~1-2 s); later opens
    find it configured and calibrated and skip that.  Opening changes no
    setting: trigger levels and hysteresis are read back from the board
    (``reset()`` applies the defaults, 0.010 V and 1000 ohm).

    simulate: None (env DANTAGGER_SIMULATE), True/'signals' (synthetic signals,
    no hardware) or 'fpga' (the protocol-level simulated FPGA).
    recalibrate: force the configure + calibrate step (also env DANTAGGER_RECALIBRATE=1).
    frame: (reference-index bits, stop bits) of the GPX2 LVDS frame; default: the
    firmware release's policy (releases 2 and 3: DDR, 24 + 18 bits at 250 MHz;
    release 1: 24 + 20 bits).  Also env DANTAGGER_FRAME=REF,STOP.
    Release 3 sends compact 4-byte records whenever the frame allows them (12 to
    24 reference-index bits, at most 18 stop bits): about twice the events over
    USB.  Env DANTAGGER_COMPACT=0 turns them off, =1 requires them.
    """
    mode = _resolve_simulate(simulate)
    if mode == "signals":
        return createSimulatedTagger()
    if int(resolution) != Resolution.Standard:
        _note("Swabian Resolution modes do not apply to the TDC board; using standard resolution. "
              "Use tagger.setHighResolution(2) or (4) for the GPX2 high-resolution modes.")
    for other in list(_open_taggers):
        if not other._t.closed() and other._t.setup_action() != "none":
            raise RuntimeError(f"the TDC board {other.getSerial()} is already open in this process; "
                               "call freeTimeTagger() on it first")
    recalibrate = recalibrate or os.environ.get("DANTAGGER_RECALIBRATE", "") == "1"
    ref_bits, stop_bits = frame or _frame()
    core = _core.open_tdc(serial=str(serial or ""), simulate_fpga=(mode == "fpga"), recalibrate=recalibrate,
                          ref_bits=ref_bits, stop_bits=stop_bits, state_dir=state_dir)
    tagger = TimeTagger(core)
    _note(f"TDC board {core.serial()}: {core.setup_message()}")
    return tagger


DEFAULT_SIM_SIGNALS = {
    1: dict(kind="detector", ref=4, efficiency=0.02, delay=31_550, jitter=20, dark_rate=200),
    2: dict(kind="detector", ref=4, efficiency=0.02, delay=31_750, jitter=20, dark_rate=200),
    3: dict(kind="poisson", rate=50_000),
    4: dict(kind="periodic", rate=1_000_000, jitter=2),
}


def createSimulatedTagger(signals=None, speed=1.0, seed=1):
    """A tagger with synthetic signals and no hardware (Dantagger only).

    signals: {channel: dict(kind=..., ...)} with kind 'poisson' (rate Hz),
    'periodic' (rate Hz, jitter ps, phase ps), 'detector' (ref channel,
    efficiency, delay ps, jitter ps, dark_rate Hz) or 'none'; every kind takes
    amplitude (V, default 1.0): no tags while the trigger level is above it.
    speed: tag time per wall time (1 = real time, <= 0 = as fast as possible).
    """
    sig = dict(DEFAULT_SIM_SIGNALS if signals is None else {})
    if signals is not None:
        sig.update({int(k): v for k, v in signals.items()})
    lst = [dict(sig.get(c, {"kind": "none"})) for c in range(1, NUM_CHANNELS + 1)]
    return TimeTagger(_core.open_sim(lst, float(speed), int(seed)))


def createReplayTagger(times, channels, speed=-1.0):
    """A tagger that replays the given tags (time-ordered ps, channels 1-4) from time 0 (Dantagger only;
    used by the conformance tests).  The tags stay in memory; nothing is written anywhere."""
    times = np.ascontiguousarray(times, dtype=np.int64)
    channels = np.ascontiguousarray(channels, dtype=np.int8)
    return TimeTagger(_core.open_replay(times, channels, float(speed)))


def freeTimeTagger(tagger):
    """Stops streaming, switches the LVDS clock off and releases the board.
    The board stays configured and calibrated for the next script."""
    if isinstance(tagger, _SyncProxy):
        tagger = tagger._sync._tagger
    if not isinstance(tagger, TimeTagger):
        raise TypeError("freeTimeTagger expects a Dantagger TimeTagger")
    if tagger._t.closed():
        return True
    tagger._t.close()
    _pump(tagger)
    _summary(tagger)
    _open_taggers.discard(tagger)
    return True


def scanTimeTagger():
    """Serial numbers of the attached TDC boards (FT601 devices)."""
    try:
        return tuple(_core.list_devices())
    except Exception:
        return ()


def _current_tagger(tagger):
    if tagger is not None:
        return tagger
    open_ = [t for t in _open_taggers if not t._t.closed()]
    if len(open_) != 1:
        raise RuntimeError("pass the tagger: there is not exactly one open Dantagger tagger")
    return open_[0]


def setHysteresis(channel, value, tagger=None):
    """Dantagger.setHysteresis(channel, ohms): comparator hysteresis of the open board (see TimeTagger.setHysteresis)."""
    return _current_tagger(tagger).setHysteresis(channel, value)


def setHighResolution(factor, tagger=None):
    """Dantagger.setHighResolution(1|2|4): TDC-GPX2 HIGH_RESOLUTION mode of the open board."""
    return _current_tagger(tagger).setHighResolution(factor)


# ---------------------------------------------------------------------------
# Synchronized measurements
# ---------------------------------------------------------------------------
class _SyncProxy:
    """What SynchronizedMeasurements.getTagger() returns: pass it to measurement
    constructors; tagger methods are forwarded to the real tagger."""

    def __init__(self, sync):
        self._sync = sync

    def __getattr__(self, name):
        return getattr(self._sync._tagger, name)

    def __repr__(self):
        return f"<Dantagger synchronized proxy of {self._sync._tagger!r}>"


def _tagger_of(tagger):
    if isinstance(tagger, _SyncProxy):
        return tagger._sync._tagger, tagger._sync
    if isinstance(tagger, TimeTagger):
        return tagger, None
    raise TypeError("expected a Dantagger TimeTagger (or SynchronizedMeasurements.getTagger())")


def _wait(fn, timeout):
    """fn(slice_ms) -> bool; waits in slices so Ctrl+C works and warnings print."""
    timeout = -1 if timeout is None else float(timeout)
    deadline = None if timeout < 0 else time.monotonic() + timeout / 1000.0
    while True:
        if deadline is None:
            slice_ms = 50
        else:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return bool(fn(0))
            slice_ms = max(0, min(50, int(remaining * 1000)))
        if fn(slice_ms):
            return True


class SynchronizedMeasurements:
    def __init__(self, tagger):
        self._tagger, sync = _tagger_of(tagger)
        if sync is not None:
            raise TypeError("SynchronizedMeasurements needs the TimeTagger itself, not a proxy")
        self._members = []   # weak references to measurements

    def getTagger(self):
        return _SyncProxy(self)

    def _handles(self):
        alive = []
        for r in self._members:
            m = r()
            if m is not None and m._h is not None:
                alive.append(m)
        self._members = [weakref.ref(m) for m in alive]
        return [m._h for m in alive]

    def registerMeasurement(self, measurement):
        if all(r() is not measurement for r in self._members):
            self._members.append(weakref.ref(measurement))

    def unregisterMeasurement(self, measurement):
        self._members = [r for r in self._members if r() is not measurement and r() is not None]

    def start(self):
        hs = self._handles()
        if hs:
            _core.group_start(hs, -1, False)

    def startFor(self, capture_duration, clear=True):
        d = _ps(capture_duration, "capture_duration")
        if d < 0:
            raise ValueError("capture_duration must be >= 0")
        hs = self._handles()
        if hs:
            _core.group_start(hs, d, bool(clear))

    def stop(self):
        hs = self._handles()
        if hs:
            _core.group_stop(hs)

    def clear(self):
        hs = self._handles()
        if hs:
            _core.group_clear(hs)

    def isRunning(self):
        hs = self._handles()
        return bool(hs) and _core.group_running(hs)

    def waitUntilFinished(self, timeout=-1):
        hs = self._handles()
        if not hs:
            return True
        return _wait(lambda ms: _core.group_wait(hs, ms), timeout)


# ---------------------------------------------------------------------------
# Measurements
# ---------------------------------------------------------------------------
class _Measurement:
    _kind = "Measurement"

    def _init(self, tagger, make):
        real, sync = _tagger_of(tagger)
        if real._t.closed():
            raise RuntimeError("the time tagger has been freed")
        self._tagger = real
        self._h = make(real._t, sync is None)   # Swabian: measurements start at creation unless synchronized
        if sync is not None:
            sync.registerMeasurement(self)

    def start(self):
        self._h.start()

    def startFor(self, capture_duration, clear=True):
        d = _ps(capture_duration, "capture_duration")
        if d < 0:
            raise ValueError("capture_duration must be >= 0")
        self._h.start_for(d, bool(clear))

    def stop(self):
        self._h.stop()

    def clear(self):
        self._h.clear()

    def abort(self):
        self._h.stop()
        self._h.clear()

    def isRunning(self):
        return self._h.is_running()

    def waitUntilFinished(self, timeout=-1):
        return _wait(self._h.wait, timeout)

    def getCaptureDuration(self):
        return int(self._h.capture_duration())

    def getConfiguration(self):
        return json.dumps({"name": self._kind, **getattr(self, "_config", {})})

    def __del__(self):
        h = getattr(self, "_h", None)
        if h is not None:
            try:
                h.detach()
            except Exception:
                pass


class Countrate(_Measurement):
    """Average count rate per channel over the capture duration."""
    _kind = "Countrate"

    def __init__(self, tagger, channels):
        chs = _channels(channels)
        self._config = {"channels": chs}
        self._init(tagger, lambda t, auto: _core.Countrate(t, chs, auto))

    def getData(self):
        return self._h.data()

    def getCountsTotal(self):
        return self._h.counts_total()


class Counter(_Measurement):
    """Time trace of counts per channel in bins of `binwidth` (ps), n_values bins kept."""
    _kind = "Counter"

    def __init__(self, tagger, channels, binwidth=1000000000, n_values=1):
        chs = _channels(channels)
        bw, n = _ps(binwidth, "binwidth"), _int(n_values, "n_values")
        self._config = {"channels": chs, "binwidth": bw, "n_values": n}
        self._init(tagger, lambda t, auto: _core.Counter(t, chs, bw, n, auto))

    def getData(self, rolling=True):
        return self._h.data(bool(rolling))

    def getDataNormalized(self, rolling=True):
        return self._h.data_normalized(bool(rolling))

    def getIndex(self):
        return self._h.index()

    def getDataTotalCounts(self):
        return self._h.total_counts()


class Correlation(_Measurement):
    """Histogram of t(channel_1) - t(channel_2), both signs; bins are [index, index + binwidth)."""
    _kind = "Correlation"

    def __init__(self, tagger, channel_1, channel_2=CHANNEL_UNUSED, binwidth=1000, n_bins=1000):
        c1 = _channel(channel_1, "channel_1")
        c2 = c1 if _int(channel_2, "channel_2") == CHANNEL_UNUSED else _channel(channel_2, "channel_2")
        bw, n = _ps(binwidth, "binwidth"), _int(n_bins, "n_bins")
        self._config = {"channel_1": c1, "channel_2": c2, "binwidth": bw, "n_bins": n}
        self._init(tagger, lambda t, auto: _core.Correlation(t, c1, c2, bw, n, auto))

    def getData(self):
        return self._h.data()

    def getDataNormalized(self):
        return self._h.data_normalized()

    def getIndex(self):
        return self._h.index()


class Histogram(_Measurement):
    """Multiple-start multiple-stop histogram of t(click) - t(start) in [0, n_bins*binwidth)."""
    _kind = "Histogram"

    def __init__(self, tagger, click_channel, start_channel=CHANNEL_UNUSED, binwidth=1000, n_bins=1000):
        click = _channel(click_channel, "click_channel")
        start = click if _int(start_channel, "start_channel") == CHANNEL_UNUSED else _channel(start_channel, "start_channel")
        bw, n = _ps(binwidth, "binwidth"), _int(n_bins, "n_bins")
        self._config = {"click_channel": click, "start_channel": start, "binwidth": bw, "n_bins": n}
        self._init(tagger, lambda t, auto: _core.Histogram(t, click, start, bw, n, auto))

    def getData(self):
        return self._h.data()

    def getIndex(self):
        return self._h.index()


class HistogramND(_Measurement):
    """Single-start, first-stop-per-axis N-dimensional histogram; getData() is flat, row-major."""
    _kind = "HistogramND"

    def __init__(self, tagger, start_channel, stop_channels, binwidths, n_bins):
        start = _channel(start_channel, "start_channel")
        stops = _channels(stop_channels, "stop_channels")
        bws = [_ps(b, "binwidths") for b in (binwidths if np.ndim(binwidths) else [binwidths])]
        ns = [_int(n, "n_bins") for n in (n_bins if np.ndim(n_bins) else [n_bins])]
        self._config = {"start_channel": start, "stop_channels": stops, "binwidths": bws, "n_bins": ns}
        self._init(tagger, lambda t, auto: _core.HistogramND(t, start, stops, bws, ns, auto))

    def getData(self):
        return self._h.data()

    def getIndex(self, dim=0):
        return self._h.index(_int(dim, "dim"))


class TimeTagStreamBuffer:
    """What TimeTagStream.getData() returns (all arrays copies, in memory only)."""

    def __init__(self, buf):
        self._timestamps = buf.timestamps()
        self._channels = buf.channels()
        self._types = buf.types()
        self._missed = buf.missed()
        self.size = int(buf.size())
        self.hasOverflows = bool(buf.has_overflows)
        self.tStart = int(buf.t_start)
        self.tGetData = int(buf.t_get_data)

    def getTimestamps(self):
        return self._timestamps

    def getChannels(self):
        return self._channels

    def getEventTypes(self):
        return self._types

    def getMissedEvents(self):
        return self._missed

    def getOverflows(self):
        return (self._types != 0).astype(np.uint8)


class TimeTagStream(_Measurement):
    """In-memory copy of the tags of `channels` (at most n_max_events between getData() calls)."""
    _kind = "TimeTagStream"

    def __init__(self, tagger, n_max_events, channels):
        n = _int(n_max_events, "n_max_events")
        chs = _channels(channels)
        self._config = {"n_max_events": n, "channels": chs}
        self._init(tagger, lambda t, auto: _core.TimeTagStream(t, n, chs, auto))

    def getData(self):
        buf = self._h.take()
        if buf.lost:
            _banner(_TITLES["stream_full"],
                    f"{buf.lost} tags were discarded because more than n_max_events="
                    f"{self._config['n_max_events']} arrived between getData() calls.")
        return TimeTagStreamBuffer(buf)

    def getCounts(self):
        return int(self._h.counts())


# ---------------------------------------------------------------------------
# Swabian names that the scripts import but never use: present so the import
# lines work; constructing one says it is not implemented.
# ---------------------------------------------------------------------------
def _not_implemented(name):
    def __init__(self, *args, **kwargs):
        raise NotImplementedError(f"{name} is not implemented in Dantagger (only the measurements the SDE "
                                  f"scripts use are; see Dantagger/SWABIAN_API_USAGE.md)")
    return type(name, (), {"__init__": __init__, "__doc__": f"Not implemented in Dantagger."})


for _name in ("Combiner", "Coincidence", "Coincidences", "Combinations", "Scope", "Event", "TimeDifferences",
              "TimeDifferencesND", "HistogramLogBins", "Histogram2D", "StartStop", "DelayedChannel",
              "GatedChannel", "FileWriter", "FileReader", "Dump", "Flim", "FrequencyCounter", "CountBetweenMarkers",
              "ConstantFractionDiscriminator", "EventGenerator", "TriggerOnCountrate", "Sampler",
              "CustomMeasurement"):
    globals()[_name] = _not_implemented(_name)
del _name
