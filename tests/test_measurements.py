"""Dantagger measurements against the brute-force references (bit for bit).

Random streams with clustering, exact ties across channels, correlated
channels, delays and dead times, replayed through the full C++ pipeline
(merger, engine windows, measurements) with createReplayTagger.
"""

import numpy as np
import pytest

import Dantagger as TT
from tests import reference as ref


def random_stream(seed, n=4000, span=2_000_000_000):
    rng = np.random.default_rng(seed)
    base = np.sort(rng.integers(1, span, n))
    per = {
        4: base,                                                     # "laser"
        1: np.sort(rng.choice(base, n // 5) + rng.integers(-3000, 3000, n // 5)),
        2: np.sort(rng.choice(base, n // 4) + rng.normal(1500, 400, n // 4).astype(np.int64)),
        3: np.sort(np.concatenate([rng.integers(1, span, n // 3), rng.choice(base, 50)])),   # exact ties with 4
    }
    per = {c: np.unique(np.clip(v, 1, None)) for c, v in per.items()}
    return per


def run(per, build, duration, delays=None, deadtimes=None):
    t, ch = ref.merge(per)   # raw stream (no delay) for the replay; Dantagger applies delays itself
    tagger = TT.createReplayTagger(t, ch)
    try:
        for c, d in (delays or {}).items():
            tagger.setInputDelay(c, d)
        for c, d in (deadtimes or {}).items():
            tagger.setDeadtime(c, d)
        sync = TT.SynchronizedMeasurements(tagger)
        ms = build(sync.getTagger())
        sync.startFor(duration)
        assert sync.waitUntilFinished(20000)
        return {k: m for k, m in ms.items()}, tagger
    finally:
        pass


@pytest.mark.parametrize("seed", [1, 2, 3])
@pytest.mark.parametrize("delays", [None, {1: 2500, 2: -700, 3: 0, 4: 31_550}])
def test_histograms_and_rates(seed, delays):
    per = random_stream(seed)
    duration = 1_500_000_000
    cfg = dict(
        corr=(1, 4, 100, 120), corr_odd=(2, 3, 7, 33), corr_auto=(4, 4, 997, 50), corr_same_tie=(3, 4, 1, 11),
        hist=(1, 4, 50, 200), hist_same=(4, 4, 10_000, 64), hist_tie=(3, 4, 1, 5),
    )

    def build(p):
        m = {k: (TT.Correlation(p, *v) if k.startswith("corr") else TT.Histogram(p, *v)) for k, v in cfg.items()}
        m["nd"] = TT.HistogramND(p, 4, [2, 1], [200, 300], [40, 30])
        m["nd1"] = TT.HistogramND(p, start_channel=4, stop_channels=[3], binwidths=[1000], n_bins=[100])
        m["cr"] = TT.Countrate(p, [1, 2, 3, 4])
        m["ctr"] = TT.Counter(p, [1, 3], binwidth=100_000_000, n_values=7)
        m["ctr1"] = TT.Counter(p, [4], binwidth=duration, n_values=1)
        return m

    ms, tagger = run(per, build, duration, delays=delays)
    t, ch = ref.merge(per, delays=delays)
    keep = (t >= 0) & (t < duration)
    tw, cw = t[keep], ch[keep]
    for k, v in cfg.items():
        if k.startswith("corr"):
            h, idx = ref.correlation(tw, cw, *v)
        else:
            h, idx = ref.histogram(tw, cw, *v)
        assert np.array_equal(ms[k].getIndex(), idx), k
        assert np.array_equal(ms[k].getData(), h), k
        assert ms[k].getData().dtype == np.int32
    assert np.array_equal(ms["nd"].getData(), ref.histogram_nd(tw, cw, 4, [2, 1], [200, 300], [40, 30]))
    assert np.array_equal(ms["nd1"].getData(), ref.histogram_nd(tw, cw, 4, [3], [1000], [100]))
    rates, counts = ref.countrate(tw, cw, [1, 2, 3, 4], duration)
    assert np.array_equal(ms["cr"].getCountsTotal(), counts)
    assert np.allclose(ms["cr"].getData(), rates)
    assert ms["cr"].getCaptureDuration() == duration
    data, totals = ref.counter(tw, cw, [1, 3], 100_000_000, 7, duration)
    assert np.array_equal(ms["ctr"].getData(), data)
    assert np.array_equal(ms["ctr"].getDataTotalCounts(), totals)
    data1, totals1 = ref.counter(tw, cw, [4], duration, 1, duration)
    assert np.array_equal(ms["ctr1"].getData(), data1) and totals1[0] == np.sum(cw == 4)
    TT.freeTimeTagger(tagger)


def test_deadtime():
    per = random_stream(7)
    per[1] = np.sort(np.concatenate([per[1], per[1] + 5000, per[1] + 12000]))
    dead = {1: 10_000}
    ms, tagger = run(per, lambda p: {"cr": TT.Countrate(p, [1]), "s": TT.TimeTagStream(p, 10**6, [1])}, 2_100_000_000,
                     deadtimes=dead)
    t, ch = ref.merge(per, deadtimes=dead)
    got = ms["s"].getData().getTimestamps()
    assert np.array_equal(got, t[ch == 1])
    TT.freeTimeTagger(tagger)


def test_counter_rolling_and_wrap():
    per = {1: np.arange(1, 400) * 200_000, 2: np.array([5, 10_000_000, 10_000_001])}
    duration = 40_000_000_000
    ms, tagger = run(per, lambda p: {"a": TT.Counter(p, [1, 2], 10_000_000, 10000), "b": TT.Counter(p, [1], 10_000_000, 5)},
                     duration)
    a = ms["a"]
    assert np.array_equal(a.getData(rolling=False)[0][:9], [49, 50, 50, 50, 50, 50, 50, 50, 0])  # as Swabian
    assert np.array_equal(a.getData(rolling=False)[1][:2], [1, 2])
    assert a.getDataTotalCounts()[0] == 399
    assert np.array_equal(ms["b"].getData(), np.zeros((1, 5)))
    assert np.array_equal(a.getIndex()[:3], [0, 10_000_000, 20_000_000])
    TT.freeTimeTagger(tagger)


def test_timetagstream_and_window_edges():
    per = {1: np.array([10, 20, 999_999, 1_000_000, 1_000_001]), 2: np.array([20, 500])}
    ms, tagger = run(per, lambda p: {"s": TT.TimeTagStream(p, 100, [1, 2]), "cr": TT.Countrate(p, [1, 2])}, 1_000_000)
    buf = ms["s"].getData()
    assert np.array_equal(buf.getTimestamps(), [10, 20, 20, 500, 999_999])
    assert np.array_equal(buf.getChannels(), [1, 1, 2, 2, 1])   # tie at 20 ordered by channel
    assert buf.getTimestamps().dtype == np.int64 and buf.getChannels().dtype == np.int32
    assert buf.getEventTypes().dtype == np.uint8 and buf.getMissedEvents().dtype == np.uint16
    assert buf.size == 5 and not buf.hasOverflows and buf.tStart == 0
    assert np.array_equal(ms["cr"].getCountsTotal(), [3, 2])
    TT.freeTimeTagger(tagger)


def test_continue_without_clear_and_restart():
    # Tags pass on while no measurement runs (as on the device), so the later
    # windows start wherever the stream is: dense data keeps every 1 ms window full.
    per = {1: np.arange(1, 200_001) * 1_000_000}   # 1 per us for 200 ms
    t, ch = ref.merge(per)
    tagger = TT.createReplayTagger(t, ch)
    cr = TT.Countrate(TT.SynchronizedMeasurements(tagger).getTagger(), [1])  # proxy: no autostart
    cr.startFor(2_000_000_000)
    cr.waitUntilFinished()
    assert cr.getCountsTotal()[0] == 1999   # tags at 1..1999 us inside [0, 2 ms)
    cr.startFor(1_000_000_000, clear=False)
    cr.waitUntilFinished()
    assert cr.getCaptureDuration() == 3_000_000_000
    assert 1999 + 999 <= cr.getCountsTotal()[0] <= 1999 + 1001
    cr.startFor(1_000_000_000)   # clear=True
    cr.waitUntilFinished()
    assert cr.getCaptureDuration() == 1_000_000_000
    assert 999 <= cr.getCountsTotal()[0] <= 1001
    TT.freeTimeTagger(tagger)


def test_settings_rules():
    tagger = TT.createSimulatedTagger(speed=1.0)
    sync = TT.SynchronizedMeasurements(tagger)
    cr = TT.Countrate(sync.getTagger(), [1])
    sync.startFor(int(0.3e12))
    with pytest.raises(TT.SettingsBusyError):
        tagger.setTriggerLevel(1, 0.1)
    with pytest.raises(TT.SettingsBusyError):
        tagger.setInputDelay(1, 100)
    sync.waitUntilFinished()
    tagger.setTriggerLevel(1, 0.1)   # fine once finished
    with pytest.raises(ValueError):
        tagger.setTriggerLevel(1, -0.2)
    with pytest.raises(ValueError):
        tagger.setTriggerLevel(2, 1.6)
    with pytest.raises(ValueError):
        TT.Countrate(tagger, [5])
    assert tagger.setHysteresis(1, 10) == 75.0
    assert tagger.setHysteresis(1, 1e6) == 5075.0
    tagger.reset()
    assert tagger.getTriggerLevel(1) == pytest.approx(0.010)
    assert tagger.getHysteresis(1) == pytest.approx(1000.0)
    TT.freeTimeTagger(tagger)


def test_negative_channel_maps_to_positive(capsys):
    per = {2: np.array([100, 200, 300])}
    t, ch = ref.merge(per)
    tagger = TT.createReplayTagger(t, ch)
    s = TT.SynchronizedMeasurements(tagger)
    cr = TT.Countrate(s.getTagger(), [-2])
    s.startFor(1000)
    s.waitUntilFinished()
    assert cr.getCountsTotal()[0] == 3
    assert "FALLING EDGES NOT SUPPORTED YET, SETTING CHANNEL 2" in capsys.readouterr().err
    TT.freeTimeTagger(tagger)
