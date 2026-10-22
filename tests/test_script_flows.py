"""The SDE scripts' own call sequences, with `import TimeTagger` -> Dantagger.

Each flow runs in a subprocess with the hook active and DANTAGGER_SIMULATE=1
(synthetic signals: detector clicks on 1 and 2 locked to the 1 MHz "laser" on
4, Poisson on 3).  The sequences are copied from the scripts and from
datautilities.py; the scripts themselves need lab instruments.
"""

import os
import pathlib
import subprocess
import sys
import textwrap

import pytest

from tests.conftest import ROOT, SDE


def run_flow(code, env_extra=None):
    prelude = textwrap.dedent("""
        import sys, time, numpy as np
        sys.path.insert(0, %r)
        import Dantagger.timetagger_hook as hook; hook.activate()
        import TimeTagger
        assert TimeTagger.__name__ == "Dantagger"
        assert TimeTagger.__file__.startswith(%r), TimeTagger.__file__
    """ % (str(ROOT / "python"), str(ROOT)))
    env = dict(os.environ, DANTAGGER_SIMULATE="1", PYTHONDONTWRITEBYTECODE="1", **(env_extra or {}))   # nothing written into SDE
    r = subprocess.run([sys.executable, "-c", prelude + textwrap.dedent(code)], cwd=SDE, env=env,
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout, r.stderr


def test_datautilities_wrappers():
    out, err = run_flow("""
        import datautilities as du
        cr = du.ttCountRate(1)
        cr.setInput(0.4, 10000)
        c = cr.measureFor(0.1)
        assert 15000 < c < 25000, c            # 2 % of 1 MHz
        TimeTagger.freeTimeTagger(cr.tagger)
        h = du.ttCountRateHist(1, 4, binwidth=2500, nbins=20, startTrig=0.013, startDelay=0)
        rate, hist = h.measureBoth(0.1)
        idx = np.array(h.histogram.getIndex(), dtype=float) * 1e-3
        assert idx[1] == 2.5 and hist.sum() > 1000 and np.argmax(hist) == 12   # 31.5 ns / 2.5 ns
        TimeTagger.freeTimeTagger(h.tagger)
        one = du.ttHist1D(channel=1, startChannel=4, binwidth=100, nbins=1000, triggerLevel=0.1, startTrig=0.013, startDelay=0)
        d, b = one.measureFor(t=0.1)
        assert b[np.argmax(d)] == 31500
        TimeTagger.freeTimeTagger(one.tagger)
        auto = du.ttHist(channel1=4, channel2=4, binwidth=1000, nbins=4000, triggerLevel=0.1)
        d, b = auto.measureFor(t=0.05)
        assert abs(d[2000] - 50000) <= 1, d[2000]                       # self pairs of the 1 MHz laser
        assert abs(d[2999] + d[3000] - (d[2000] - 1)) <= 2              # +1 us neighbours (jitter splits the edge)
        assert abs(d[999] + d[1000] - (d[2000] - 1)) <= 2               # -1 us
        TimeTagger.freeTimeTagger(auto.tagger)
        print("ok")
    """)
    assert "ok" in out


def test_tomography_measure_hist_flow():
    out, err = run_flow("""
        from TimeTagger import (createTimeTagger, freeTimeTagger, Resolution, HistogramND, SynchronizedMeasurements,
                                Counter, Countrate)
        resolution_mode = Resolution.HighResB
        ch_start, ch_stop1, ch_stop2, ch_count = 4, 2, -1, 3
        nBins, totalRange = 500, 1126
        binWidth = (totalRange * 2) / nBins
        tagger = createTimeTagger()
        for channel, v in {ch_start: 0.013, ch_stop1: 0.009, ch_stop2: -0.019, ch_count: 0.5}.items():
            tagger.setTriggerLevel(channel, v)
        # The sim's detectors sit 31.5/31.7 ns after the laser: delay the start into the window.
        tagger.setDelaySoftware(ch_stop1, 0)
        tagger.setDelaySoftware(ch_stop2, 0)
        tagger.setDelaySoftware(ch_start, 31_000)
        countrate_sync = SynchronizedMeasurements(tagger)
        rate_measurement = Countrate(countrate_sync.getTagger(), [ch_count])
        countrate_sync.startFor(int(1e11))
        countrate_sync.waitUntilFinished()
        measured = float(rate_measurement.getData()[0])
        assert 40000 < measured < 60000, measured
        chunk = int(round(0.2 * 1e12))
        synchronized = SynchronizedMeasurements(tagger)
        sync_tagger = synchronized.getTagger()
        hist2d = HistogramND(sync_tagger, start_channel=ch_start, stop_channels=[ch_stop1, ch_stop2],
                             binwidths=[int(binWidth), int(binWidth)], n_bins=[int(nBins), int(nBins)])
        counter = Counter(sync_tagger, channels=[ch_count], binwidth=chunk, n_values=1)
        hc = Counter(sync_tagger, channels=[ch_start, ch_stop1, ch_stop2], binwidth=chunk, n_values=1)
        synchronized.startFor(chunk)
        synchronized.waitUntilFinished()
        data = np.array(hist2d.getData(), dtype=float, copy=True)
        assert data.shape == (nBins * nBins,) and data.sum() > 0
        totals = np.array(counter.getDataTotalCounts(), dtype=float)
        assert 8000 < totals[0] < 12000, totals
        assert np.array(hc.getData()).shape == (3, 1)
        del hc, counter, hist2d, sync_tagger, synchronized
        freeTimeTagger(tagger)
        print("ok")
    """)
    assert "ok" in out
    assert "FALLING EDGES NOT SUPPORTED YET, SETTING CHANNEL 1" in err


def test_sweep_and_polling_flows():
    out, err = run_flow("""
        from TimeTagger import HistogramND, SynchronizedMeasurements, createTimeTagger, freeTimeTagger
        tagger = createTimeTagger()
        tagger.setTriggerLevel(4, 0.5)
        synchronized = SynchronizedMeasurements(tagger)
        hist2d = HistogramND(synchronized.getTagger(), start_channel=4, stop_channels=[2, 1], binwidths=[100, 100],
                             n_bins=[400, 400])
        synchronized.stop()
        counts = []
        for trig in (0.1, 1.2):          # 1.2 V is above the sim amplitude (1.0 V): no clicks
            tagger.setTriggerLevel(2, trig)
            synchronized.startFor(int(0.05e12), clear=True)
            synchronized.waitUntilFinished()
            counts.append(int(np.sum(hist2d.getData(), dtype=np.uint64)))
        assert counts[1] == 0 and counts[0] >= 0
        # pulsePicker_streamCountrate: startFor, poll isRunning, read.
        rate = TimeTagger.Countrate(synchronized.getTagger(), [4, 3])
        synchronized.startFor(int(0.1e12), clear=True)
        polls = 0
        while synchronized.isRunning():
            time.sleep(0.01); polls += 1
        r = rate.getData()
        assert polls > 3 and abs(r[0] - 1e6) < 1e3, r
        freeTimeTagger(tagger)
        print("ok")
    """)
    assert "ok" in out


def test_stream_and_autostart_flows():
    out, err = run_flow("""
        tagger = TimeTagger.createTimeTagger()
        tagger.setTriggerLevel(3, 0.1); tagger.setDeadtime(3, 0)
        stream = TimeTagger.TimeTagStream(tagger, 10_000_000, [3])
        stream.start(); time.sleep(0.2); stream.stop()
        data = stream.getData()
        ts = data.getTimestamps(); ov = data.getOverflows()
        assert 5000 < len(ts) < 15000 and ov.sum() == 0 and np.all(np.diff(ts) > 0)
        # correlation_extinction_vs_trig: an autostarted Correlation, triggers changed between windows.
        corr = TimeTagger.Correlation(tagger, 1, 4, 100, 1000)
        for trig in (0.05, 0.1):
            tagger.setTriggerLevel(channel=4, voltage=trig)
            corr.clear(); corr.startFor(int(0.05e12)); corr.waitUntilFinished()
            idx = np.array(corr.getIndex()); c = np.array(corr.getData())
            assert idx[np.argmax(c)] == 31500, idx[np.argmax(c)]   # detector 31.55 ns after the laser
        print("ok")
        TimeTagger.freeTimeTagger(tagger)
    """)
    assert "ok" in out


def test_hook_pth_switch(tmp_path):
    """The .pth the installer writes: Dantagger by default, Swabian with SDE_TAGGER=swabian."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("inst", ROOT / "tools" / "install_timetagger_hook.py")
    inst = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inst)
    import sysconfig
    real = sysconfig.get_paths
    try:
        sysconfig.get_paths = lambda: {"purelib": str(tmp_path)}
        sys.argv = ["x"]
        inst.main()
    finally:
        sysconfig.get_paths = real
    pth = tmp_path / inst.NAME
    assert pth.exists()
    probe = "import site, sys; site.addsitedir(%r); import TimeTagger; print(TimeTagger.__name__)" % str(tmp_path)
    for env, want in (({}, "Dantagger"), ({"SDE_TAGGER": "dantagger"}, "Dantagger"), ({"SDE_TAGGER": "swabian"}, "TimeTagger")):
        e = {k: v for k, v in os.environ.items() if k != "SDE_TAGGER"}
        e.update(env)
        r = subprocess.run([sys.executable, "-c", probe], env=e, capture_output=True, text=True, cwd="/tmp", timeout=60)
        assert r.stdout.strip().splitlines()[-1] == want, r.stdout + r.stderr
