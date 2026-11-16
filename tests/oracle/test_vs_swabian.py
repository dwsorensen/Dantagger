"""Dantagger against the real Swabian library (black box, bit for bit).

The same random tag streams go into a Swabian virtual tagger (pattern
generators) and into Dantagger's replay tagger; every measurement and method
the SDE scripts use must give identical results.  Skipped where the Swabian
library (with its virtual-tagger licence) is not installed.  Slow-ish: the
Swabian virtual tagger simulates in real time.
"""

import numpy as np
import pytest

import Dantagger as DT
from tests.oracle.swabian_oracle import OracleRun, available, swabian
from tests.test_measurements import random_stream

pytestmark = pytest.mark.skipif(not available(), reason="Swabian TimeTagger library not available")

CONFIGS = dict(
    corr=("Correlation", (1, 4, 100, 120)),
    corr_odd=("Correlation", (2, 3, 7, 33)),
    corr_auto=("Correlation", (4, 4, 997, 50)),
    corr_tie=("Correlation", (3, 4, 1, 11)),
    corr_rev=("Correlation", (4, 2, 250, 64)),
    hist=("Histogram", (1, 4, 50, 200)),
    hist_same=("Histogram", (4, 4, 10_000, 64)),
    hist_tie=("Histogram", (3, 4, 1, 5)),
    hist_rev=("Histogram", (4, 2, 300, 100)),
)
ND = [((4, [2, 1], [200, 300], [40, 30])), ((4, [3], [1000], [100])), ((1, [2, 3], [500, 500], [20, 20]))]


def swabian_build(vt, ch):
    T = swabian()
    m = {}
    for k, (cls, (a, b, bw, n)) in CONFIGS.items():
        m[k] = getattr(T, cls)(vt, ch(a), ch(b), bw, n)
    for i, (s, stops, bws, ns) in enumerate(ND):
        m[f"nd{i}"] = T.HistogramND(vt, ch(s), [ch(x) for x in stops], bws, ns)
    m["cr"] = T.Countrate(vt, [ch(c) for c in (1, 2, 3, 4)])
    m["ctr"] = T.Counter(vt, [ch(1), ch(3)], 100_000_000, 5000)
    return m


def extract(m):
    out = {}
    for k, v in m.items():
        if k == "cr":
            out[k] = (v.getCountsTotal(),)
        elif k == "ctr":
            out[k] = (v.getData(rolling=False), v.getIndex())
        elif k.startswith("nd"):
            out[k] = (v.getData(), v.getIndex(0))
        else:
            out[k] = (v.getData(), v.getIndex())
        v.stop()
    return out


def dense_stream(seed, n=4000, span=1_000_000_000):
    """Tight correlations and many exact ties across all four channels."""
    rng = np.random.default_rng(seed)
    base = np.unique(rng.integers(1, span, n))
    pick = lambda k: rng.choice(base, k, replace=False)
    per = {
        4: base,
        3: np.concatenate([pick(n // 3), pick(n // 4) + rng.integers(0, 4, n // 4)]),
        2: pick(n // 3) + rng.integers(-60, 220, n // 3),
        1: np.concatenate([pick(n // 4), pick(n // 5) + rng.integers(-1000, 1000, n // 5)]),
    }
    return {c: np.unique(np.clip(v, 1, None)) for c, v in per.items()}


@pytest.mark.parametrize("stream", [("random", 11), ("random", 12), ("dense", 21), ("dense", 22)])
def test_against_swabian(stream):
    kind, seed = stream
    per = (random_stream if kind == "random" else dense_stream)(seed, n=3000, span=1_000_000_000)
    end = 1_200_000_000
    sw = OracleRun(per, swabian_build, extract, end_ps=end)
    # The tags Swabian actually saw, in its stream order.
    total = sum(len(v) for v in per.values())
    assert len(sw.times) == total
    tagger = DT.createReplayTagger(sw.times, sw.channels)
    try:
        sync = DT.SynchronizedMeasurements(tagger)
        p = sync.getTagger()
        m = {k: getattr(DT, cls)(p, *args) for k, (cls, args) in CONFIGS.items()}
        for i, (s, stops, bws, ns) in enumerate(ND):
            m[f"nd{i}"] = DT.HistogramND(p, s, stops, bws, ns)
        m["cr"] = DT.Countrate(p, [1, 2, 3, 4])
        m["ctr"] = DT.Counter(p, [1, 3], 100_000_000, 5000)
        sync.startFor(end)
        assert sync.waitUntilFinished(30000)
        for k in CONFIGS:
            want_data, want_idx = sw.results[k]
            assert np.array_equal(m[k].getIndex(), want_idx), k
            assert np.array_equal(m[k].getData(), want_data), k
            assert m[k].getData().dtype == want_data.dtype, k
        for i in range(len(ND)):
            want, want_idx = sw.results[f"nd{i}"]
            assert np.array_equal(m[f"nd{i}"].getData(), want), f"nd{i}"
            assert np.array_equal(m[f"nd{i}"].getIndex(0), want_idx)
        assert np.array_equal(m["cr"].getCountsTotal(), sw.results["cr"][0])
        # Counter: Swabian ran longer (its virtual time advances in large steps);
        # the first 12 bins (1.2 ms, all the data) must agree; 5000 bins never wrap.
        want, want_idx = sw.results["ctr"]
        got = m["ctr"].getData(rolling=False)
        assert np.array_equal(got[:, :12], want[:, :12])
        assert np.array_equal(m["ctr"].getIndex(), want_idx)
    finally:
        DT.freeTimeTagger(tagger)


# Coincidence virtual channels: groups (Dantagger channels) and windows; the
# last one reads the first virtual channel ("chain").
COINC = [([1, 2], 3000), ([4, 1], 2500), ([1, 2, 3], 6000), ([3, 4], 1), ([2, 4], 1500), ([3, 3], 1000), ("chain", 4000)]


@pytest.mark.parametrize("stream", [("random", 31), ("dense", 32)])
def test_coincidence_against_swabian(stream):
    kind, seed = stream
    per = (random_stream if kind == "random" else dense_stream)(seed, n=3000, span=1_000_000_000)
    end = 1_200_000_000

    def build(vt, ch):
        T = swabian()
        co = []
        for g, w in COINC:
            chans = [co[0].getChannel(), ch(4)] if g == "chain" else [ch(c) for c in g]
            co.append(T.Coincidence(vt, chans, w))
        vch = [c.getChannel() for c in co]
        return {"co": co, "vch": vch, "s": T.TimeTagStream(vt, 10**6, vch), "cr": T.Countrate(vt, vch)}

    def extract(o):
        b = o["s"].getData()
        return {"t": b.getTimestamps().copy(), "c": b.getChannels().copy(), "vch": o["vch"],
                "counts": o["cr"].getCountsTotal().copy()}

    sw = OracleRun(per, build, extract, end_ps=end)
    r = sw.results
    tagger = DT.createReplayTagger(sw.times, sw.channels)
    try:
        co = []
        for g, w in COINC:
            co.append(DT.Coincidence(tagger, [co[0].getChannel(), 4] if g == "chain" else g, w))
        vch = [c.getChannel() for c in co]
        sync = DT.SynchronizedMeasurements(tagger)
        s = DT.TimeTagStream(sync.getTagger(), 10**6, vch)
        sync.startFor(end)
        assert sync.waitUntilFinished(30000)
        b = s.getData()
        got_t, got_c = b.getTimestamps(), b.getChannels()
        total = 0
        for k in range(len(COINC)):
            want = r["t"][r["c"] == r["vch"][k]]
            want = want[want < end]
            got = got_t[got_c == vch[k]]
            assert np.array_equal(got, want), COINC[k]
            total += len(want)
        assert total > 300
    finally:
        DT.freeTimeTagger(tagger)
