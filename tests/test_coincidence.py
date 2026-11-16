"""Coincidence / Coincidences virtual channels against the brute-force
reference (tests/reference.coincidence) and the cases measured on the Swabian
library (2.22.6, virtual tagger; tests/oracle/test_vs_swabian.py runs random
streams against it too).
"""

import json

import numpy as np
import pytest

import Dantagger as TT
from tests import reference as ref
from tests.test_measurements import random_stream

# (channel, offset ps) -> the coincidence tags Swabian made: (group, offset).
# Groups: "12" = [1, 2], "123" = [1, 2, 3], window 1000 ps.
SWABIAN_CASES = {
    "A": ([(1, 0), (2, 500)], [("12", 500)]),
    "B": ([(1, 0), (2, 1000)], []),                       # exactly the window: no
    "C": ([(1, 0), (2, 1001)], []),
    "D": ([(1, 0), (2, 500), (2, 700)], [("12", 500), ("12", 700)]),   # nothing is used up
    "E": ([(1, 0), (1, 300), (2, 500)], [("12", 500)]),
    "F": ([(1, 0), (2, 0)], [("12", 0)]),                 # the same time: one
    "G": ([(2, 0), (1, 500)], [("12", 500)]),
    "H": ([(1, 0), (2, 500), (1, 800), (2, 1200)], [("12", 500), ("12", 800), ("12", 1200)]),
    "I": ([(1, 0), (2, 500), (1, 600)], [("12", 500), ("12", 600)]),
    "J": ([(1, 0), (2, 500), (1, 1400)], [("12", 500), ("12", 1400)]),   # the latest 2 is within 1 ns
    "K": ([(1, 0), (2, 500), (3, 900)], [("12", 500), ("123", 900)]),
    "L": ([(1, 0), (2, 500), (3, 1100)], [("12", 500)]),
    "M": ([(1, 0), (2, 500), (3, 900), (3, 950)], [("12", 500), ("123", 900), ("123", 950)]),
    # Both made by the tag at 950: each virtual tag goes right after the tag
    # that made it, so the later virtual channel's comes first (as Swabian).
    "N": ([(1, 0), (2, 500), (3, 900), (1, 950)], [("12", 500), ("123", 900), ("123", 950), ("12", 950)]),
}


def replay(per):
    t, ch = ref.merge(per)
    return TT.createReplayTagger(t, ch)


def run_sync(tagger, build, duration):
    sync = TT.SynchronizedMeasurements(tagger)
    ms = build(sync.getTagger())
    sync.startFor(duration)
    assert sync.waitUntilFinished(20000)
    return ms


def test_swabian_cases():
    per = {1: [], 2: [], 3: []}
    starts = {}
    for k, (name, (events, _)) in enumerate(SWABIAN_CASES.items()):
        base = 1_000_000 + k * 10_000_000
        starts[name] = base
        for c, o in events:
            per[c].append(base + o)
    tagger = replay({c: np.array(v) for c, v in per.items()})
    c12 = TT.Coincidence(tagger, [1, 2], 1000)
    c123 = TT.Coincidence(tagger, [1, 2, 3], coincidenceWindow=1000, timestamp=TT.CoincidenceTimestamp.Last)
    assert (c12.getChannel(), c123.getChannel()) == (5, 6)   # after the inputs, as Swabian numbers them
    names = {5: "12", 6: "123"}
    ms = run_sync(tagger, lambda p: {"s": TT.TimeTagStream(p, 1000, [5, 6]), "cr": TT.Countrate(p, [5, 6])},
                  len(SWABIAN_CASES) * 10_000_000 + 2_000_000)
    buf = ms["s"].getData()
    t, c = buf.getTimestamps(), buf.getChannels()
    for name, base in starts.items():
        sel = (t >= base) & (t < base + 5_000_000)
        got = [(names[int(ch)], int(x - base)) for x, ch in zip(t[sel], c[sel])]
        assert got == SWABIAN_CASES[name][1], name
    want = [sum(g == n for v in SWABIAN_CASES.values() for g, _ in v[1]) for n in ("12", "123")]
    assert list(ms["cr"].getCountsTotal()) == want
    TT.freeTimeTagger(tagger)


GROUPS = [([1, 2], 3000), ([4, 1], 2500), ([2, 4], 1500), ([1, 2, 3], 6000), ([3, 4], 1), ([3, 4], 0), ([2, 2], 1000),
          ([3], 1000), ([4, 3, 1], 4000)]


@pytest.mark.parametrize("seed", [1, 2])
@pytest.mark.parametrize("delays", [None, {1: 2500, 2: -700, 4: 1500}])
def test_against_reference(seed, delays):
    per = random_stream(seed)
    duration = 1_500_000_000
    t, ch = ref.merge(per)
    tagger = TT.createReplayTagger(t, ch)
    for c, d in (delays or {}).items():
        tagger.setInputDelay(c, d)
    co = [TT.Coincidence(tagger, g, w) for g, w in GROUPS]
    vch = [x.getChannel() for x in co]
    assert vch == list(range(5, 5 + len(GROUPS)))

    def build(p):
        return {"cr": TT.Countrate(p, vch + [1, 2, 3, 4]),
                "s": TT.TimeTagStream(p, 10**6, vch),
                "ctr": TT.Counter(p, [vch[0], vch[3]], 100_000_000, 20),
                "corr": TT.Correlation(p, vch[0], 4, 100, 200),
                "hist": TT.Histogram(p, vch[1], 4, 50, 100)}

    ms = run_sync(tagger, build, duration)
    tm, cm = ref.merge(per, delays=delays)   # the whole stream: the virtual channels see the tags before the window too
    tm, cm = tm[tm >= 0], cm[tm >= 0]        # a negative delay drops tags before the run's start
    buf = ms["s"].getData()
    got_t, got_c = buf.getTimestamps(), buf.getChannels()
    counts = ms["cr"].getCountsTotal()
    tw, cw = tm[(tm >= 0) & (tm < duration)], cm[(tm >= 0) & (tm < duration)]
    full_t, full_c = tm, cm   # the stream with every virtual channel's tags, as the measurements see it
    for k, (g, w) in enumerate(GROUPS):
        at = ref.coincidence_at(full_t, full_c, g, w)
        want = full_t[at]
        want = want[(want >= 0) & (want < duration)]
        assert np.array_equal(got_t[got_c == vch[k]], want), (g, w)
        assert counts[k] == len(want), (g, w)
        full_t, full_c = ref.insert_after(full_t, full_c, at, vch[k])
    keep = full_t < duration
    full_t, full_c = full_t[keep], full_c[keep]
    assert counts[-4:].tolist() == [int(np.sum(cw == c)) for c in (1, 2, 3, 4)]   # the inputs are unchanged
    assert GROUPS[5][1] == 0 and counts[5] == 0                                    # window 0 never fires
    assert counts[7] == np.sum(cw == 3)                                            # a group of one copies its channel
    # Measurements on virtual channels as on inputs.
    data, totals = ref.counter(full_t, full_c, [vch[0], vch[3]], 100_000_000, 20, duration)
    assert np.array_equal(ms["ctr"].getData(), data) and np.array_equal(ms["ctr"].getDataTotalCounts(), totals)
    h, idx = ref.correlation(full_t, full_c, vch[0], 4, 100, 200)
    assert np.array_equal(ms["corr"].getData(), h) and np.array_equal(ms["corr"].getIndex(), idx)
    h, idx = ref.histogram(full_t, full_c, vch[1], 4, 50, 100)
    assert np.array_equal(ms["hist"].getData(), h)
    assert sum(counts[:len(GROUPS)]) > 500   # the streams make plenty of coincidences
    TT.freeTimeTagger(tagger)


def test_chained_and_coincidences():
    per = random_stream(5)
    t, ch = ref.merge(per)
    tagger = TT.createReplayTagger(t, ch)
    cs = TT.Coincidences(tagger, [[1, 4], [2, 4]], 3000)
    a, b = cs.getChannels()
    chained = TT.Coincidence(tagger, [a, 2], 4000)   # (1 and 4) and 2
    duration = 1_800_000_000
    ms = run_sync(tagger, lambda p: {"s": TT.TimeTagStream(p, 10**6, [a, b, chained.getChannel()])}, duration)
    buf = ms["s"].getData()
    got_t, got_c = buf.getTimestamps(), buf.getChannels()
    at_a = ref.coincidence_at(t, ch, [1, 4], 3000)
    t2, c2 = ref.insert_after(t, ch, at_a, a)
    at_b = ref.coincidence_at(t2, c2, [2, 4], 3000)
    t3, c3 = ref.insert_after(t2, c2, at_b, b)
    wa, wb = t[at_a], t2[at_b]
    wc = ref.coincidence(t3, c3, [a, 2], 4000)
    for want, c in ((wa, a), (wb, b), (wc, chained.getChannel())):
        want = want[want < duration]
        assert np.array_equal(got_t[got_c == c], want)
    assert len(wc) > 10
    TT.freeTimeTagger(tagger)


def test_stream_order_ties():
    # A coincidence tag follows the tag that made it, also before inputs at the same time (as Swabian).
    per = {1: np.array([100, 5000]), 2: np.array([100, 5200]), 3: np.array([100, 5200])}
    tagger = replay(per)
    c = TT.Coincidence(tagger, [1, 2], 1000)
    d = TT.Coincidence(tagger, [2, 1], 1000)   # made by the same tags: its tag comes first
    ms = run_sync(tagger, lambda p: {"s": TT.TimeTagStream(p, 100, [1, 2, 3, 5, 6])}, 10_000)
    buf = ms["s"].getData()
    assert buf.getTimestamps().tolist() == [100, 100, 100, 100, 100, 5000, 5200, 5200, 5200, 5200]
    assert buf.getChannels().tolist() == [1, 2, 6, 5, 3, 1, 2, 6, 5, 3]
    assert (c.getChannel(), d.getChannel()) == (5, 6)
    TT.freeTimeTagger(tagger)


def test_stop_start_window_and_clear():
    # One pair per us for 200 ms, 500 ps apart (the replay runs on between
    # windows, as the board does, so every 2 ms window is full).
    per = {1: np.arange(1, 200_001) * 1_000_000, 2: np.arange(1, 200_001) * 1_000_000 + 500}
    tagger = replay(per)
    c = TT.Coincidence(tagger, [1, 2], 1000)
    assert c.isRunning()
    c.setCoincidenceWindow(500)                     # exactly 500 ps apart: nothing
    cr = TT.Countrate(TT.SynchronizedMeasurements(tagger).getTagger(), [c.getChannel(), 1])
    cr.startFor(2_000_000_000)
    cr.waitUntilFinished()
    assert cr.getCountsTotal()[0] == 0 and cr.getCountsTotal()[1] >= 1999
    c.setCoincidenceWindow(501)
    cr.startFor(2_000_000_000)
    cr.waitUntilFinished()
    assert 1999 <= cr.getCountsTotal()[0] <= 2001
    c.stop()
    assert not c.isRunning()
    cr.startFor(2_000_000_000)
    cr.waitUntilFinished()
    assert cr.getCountsTotal()[0] == 0 and cr.getCountsTotal()[1] >= 1999
    c.start()
    c.clear()
    cr.startFor(2_000_000_000)
    cr.waitUntilFinished()
    assert 1999 <= cr.getCountsTotal()[0] <= 2001
    cfg = json.loads(c.getConfiguration())
    assert cfg["name"] == "Coincidence" and cfg["coincidenceWindow"] == 501 and cfg["groups"] == [[1, 2]]
    TT.freeTimeTagger(tagger)


def test_arguments_and_numbers():
    tagger = replay({1: np.array([10])})
    with pytest.raises(ValueError):
        TT.Coincidence(tagger, [1, 2], -1)
    with pytest.raises(ValueError):
        TT.Coincidences(tagger, [[1, 2], []], 1000)
    with pytest.raises(ValueError):
        TT.Coincidence(tagger, [1, 9], 1000)            # no such virtual channel
    with pytest.raises(ValueError):
        TT.Coincidence(tagger, [1, 32], 1000)
    with pytest.raises(NotImplementedError):
        TT.Coincidence(tagger, [1, 2], 1000, TT.CoincidenceTimestamp.First)
    with pytest.raises(ValueError):
        TT.Countrate(tagger, [5])                       # before any Coincidence: channel 5 does not exist
    a = TT.Coincidence(tagger, [1, 2])
    b = TT.Coincidence(tagger, [1, 3], 2000)
    assert (a.getChannel(), b.getChannel()) == (5, 6)
    TT.Countrate(TT.SynchronizedMeasurements(tagger).getTagger(), [5, 6])   # now they exist
    with pytest.raises(ValueError):
        tagger.setInputDelay(5, 100)                    # virtual channels have no input settings
    with pytest.raises(ValueError):
        TT.Countrate(tagger, [-5])
    del a
    c = TT.Coincidence(tagger, [3, 4])
    assert c.getChannel() == 5                          # a freed number is used again
    more = [TT.Coincidence(tagger, [1, 2]) for _ in range(25)]
    assert [m.getChannel() for m in more] == list(range(7, 32))
    with pytest.raises(RuntimeError):
        TT.Coincidence(tagger, [1, 2])                  # 27 at a time
    del more[0]                                         # frees 7
    chained = TT.Coincidence(tagger, [b.getChannel(), 1])
    assert chained.getChannel() == 7
    del b                                               # 6 is still read by `chained`: not handed out again
    with pytest.raises(RuntimeError):
        TT.Coincidence(tagger, [1, 2])
    TT.freeTimeTagger(tagger)


def test_simulated_detectors():
    # The default simulated signals: channels 1 and 2 detect 2 % of a 1 MHz
    # laser (channel 4) each, 200 ps apart: about 400 coincidences/s, plus
    # 0.8/s accidentals in 1 ns; channel 3 (Poisson) coincides with neither.
    tagger = TT.createSimulatedTagger(speed=0)
    c12 = TT.Coincidence(tagger, [1, 2], 1000)
    c13 = TT.Coincidence(tagger, [1, 3], 1000)
    c12_narrow = TT.Coincidence(tagger, [1, 2], 100)   # 200 ps apart: almost none
    sync = TT.SynchronizedMeasurements(tagger)
    cr = TT.Countrate(sync.getTagger(), [c12.getChannel(), c13.getChannel(), c12_narrow.getChannel()])
    sync.startFor(int(2e12))
    assert sync.waitUntilFinished(60000)
    n12, n13, narrow = cr.getCountsTotal()
    assert 700 < n12 < 900, n12
    assert n13 < 15 and narrow < 10, (n13, narrow)   # n13: about 4 accidentals expected
    TT.freeTimeTagger(tagger)
