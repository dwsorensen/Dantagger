"""The hardware backend through tdcctl's simulated FPGA (protocol level).

createTimeTagger(simulate="fpga") runs the real TDC backend (lifecycle,
comparator commands, LVDS calibration, streaming, record decoding) against a
simulated FPGA that persists within the process like a plugged-in board.
"""

import numpy as np
import pytest

import Dantagger as TT
from Dantagger import _dantagger as core


@pytest.fixture
def board(tmp_path):
    core.sim_fpga_unplug()
    core.sim_fpga_set(event_rate_hz=20000.0)
    yield str(tmp_path)
    core.sim_fpga_unplug()


def open_board(state_dir, **kw):
    return TT.createTimeTagger(simulate="fpga", state_dir=state_dir, **kw)


def test_lifecycle_configure_and_calibrate_once(board, capsys):
    t = open_board(board)
    assert t._t.setup_action() == "calibrated"
    TT.freeTimeTagger(t)
    t = open_board(board)                       # "another script, board not unplugged"
    assert t._t.setup_action() == "skipped"
    t.setHighResolution(4)
    TT.freeTimeTagger(t)
    t = open_board(board)                       # defaults again: GPX2 reconfigured, no recalibration
    assert t._t.setup_action() == "configured"
    assert t.getHighResolution() == 1
    TT.freeTimeTagger(t)
    core.sim_fpga_unplug()                      # unplug / power cycle
    t = open_board(board)
    assert t._t.setup_action() == "calibrated"
    TT.freeTimeTagger(t)
    t = open_board(board, recalibrate=True)
    assert t._t.setup_action() == "calibrated"
    TT.freeTimeTagger(t)
    err = capsys.readouterr().err
    assert "already configured and calibrated" in err and "first open since the board was plugged in" in err


def test_open_applies_nothing_and_comparator_commands(board):
    """Opening reads the comparator back and changes nothing (user, 2026-10-22);
    reset() applies the defaults."""
    code_10mv = round((0.010 + 2.5 / 24.9) * 65536 / 2.5)
    t = open_board(board)
    st = core.sim_fpga_state()
    assert st["dac"] == [0] * 4                        # the simulated board's power-up state, untouched
    assert st["wiper"] == [128] * 4
    assert t.getTriggerLevel(1) == pytest.approx(-2.5 / 24.9, abs=1e-4)   # read back: DAC code 0
    t.setTriggerLevel(2, 0.25)
    assert core.sim_fpga_state()["dac"][1] == round((0.25 + 2.5 / 24.9) * 65536 / 2.5)
    assert TT.setHysteresis(3, 2500) == pytest.approx(2500, abs=20)   # module-level form
    assert core.sim_fpga_state()["wiper"][2] == round((2500 - 75) * 256 / 5000)
    assert t.setHysteresis(4, 0) == 75.0
    assert not core.sim_fpga_state()["lclk_en"]
    TT.freeTimeTagger(t)
    t = open_board(board)                              # another script: the settings are still there
    assert t._t.setup_action() == "skipped"
    assert t.getTriggerLevel(2) == pytest.approx(0.25, abs=1e-4)
    assert t.getHysteresis(3) == pytest.approx(2500, abs=20)
    assert core.sim_fpga_state()["dac"][1] == round((0.25 + 2.5 / 24.9) * 65536 / 2.5)
    t.reset()                                          # Swabian reset(): the defaults
    assert core.sim_fpga_state()["dac"] == [code_10mv] * 4
    assert core.sim_fpga_state()["wiper"] == [round((1000 - 75) * 256 / 5000)] * 4
    assert t.getTriggerLevel(1) == pytest.approx(0.010, abs=1e-4)
    TT.freeTimeTagger(t)


def test_stream_only_while_measuring_and_rates(board):
    t = open_board(board)
    s = TT.SynchronizedMeasurements(t)
    cr = TT.Countrate(s.getTagger(), [1, 2, 3, 4])
    c = TT.Correlation(s.getTagger(), 1, 2, 1000, 200)
    assert not core.sim_fpga_state()["lclk_en"] and not t._t.streaming()
    s.startFor(int(0.5e12))
    assert s.waitUntilFinished(20000)
    rates = cr.getData()
    assert np.all(np.abs(rates - 20000) < 2000), rates      # 20 kHz Poisson per channel
    assert cr.getCaptureDuration() == int(0.5e12)
    assert c.getData().sum() > 0
    import time
    for _ in range(100):
        if not t._t.streaming():
            break
        time.sleep(0.01)
    assert not t._t.streaming() and not core.sim_fpga_state()["lclk_en"]
    TT.freeTimeTagger(t)


def test_overflow_banner_and_summary(board, capsys):
    core.sim_fpga_set(event_rate_hz=200000.0, drop_every=50)
    t = open_board(board)
    s = TT.SynchronizedMeasurements(t)
    cr = TT.Countrate(s.getTagger(), [1])
    s.startFor(int(0.2e12))
    s.waitUntilFinished()
    TT.freeTimeTagger(t)
    err = capsys.readouterr().err
    assert "DANTAGGER OVERFLOW" in err and "EVENTS WERE LOST" in err
    assert "DANTAGGER SESSION SUMMARY: DATA WAS LOST" in err
    assert t._t.dropped_total() > 0


def test_settings_blocked_while_running(board):
    t = open_board(board)
    s = TT.SynchronizedMeasurements(t)
    cr = TT.Countrate(s.getTagger(), [1])
    s.startFor(int(5e12))
    with pytest.raises(TT.SettingsBusyError):
        t.setTriggerLevel(1, 0.2)
    with pytest.raises(TT.SettingsBusyError):
        t.setHysteresis(1, 2000)
    with pytest.raises(TT.SettingsBusyError):
        t.setHighResolution(2)
    s.stop()
    t.setTriggerLevel(1, 0.2)
    TT.freeTimeTagger(t)
