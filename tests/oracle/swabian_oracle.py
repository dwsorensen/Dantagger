"""Run the real Swabian library on chosen time tags (black-box oracle).

The Swabian virtual tagger (``createTimeTaggerVirtual``) plus one
``Experimental.PatternSignalGenerator`` per channel injects exactly the tags we
choose; Swabian measurements run on them; a ``TimeTagStream`` captures what the
virtual tagger actually delivered so the same tags can be fed to Dantagger.

The virtual tagger simulates in real time, so keep streams short (< ~0.3 s of
tag time).  Requires the Swabian ``TimeTagger`` package and its virtual-tagger
licence (present on the lab PC).
"""

import time

import numpy as np


def swabian():
    """The Swabian module itself, also while `import TimeTagger` gives Dantagger."""
    try:
        import sde_timetagger_switch
    except ImportError:
        import TimeTagger
        if TimeTagger.__name__ == "Dantagger":
            raise ImportError("TimeTagger is aliased to Dantagger and the switch module is not available")
        return TimeTagger
    return sde_timetagger_switch.import_swabian()


def available():
    try:
        return hasattr(swabian(), "createTimeTaggerVirtual")
    except Exception:
        return False


class OracleRun:
    """tags: {dantagger_channel: sorted int64 array of ps (all > 0)}.

    ``build(vt, ch)`` creates Swabian measurements; ``ch(n)`` maps a
    Dantagger channel number to the virtual channel.  It returns any object
    (dict of measurements); ``extract(objs)`` turns them into plain numpy
    results after the stream has passed ``end_ps``.
    """

    def __init__(self, tags, build, extract, end_ps, after_run=None, delays=None):
        T = swabian()
        self.T = T
        vt = T.createTimeTaggerVirtual()
        gens, vmap = [], {}
        for c, times in sorted(tags.items()):
            times = np.asarray(times, dtype=np.int64)
            seq = [int(x) for x in times] if len(times) else [int(end_ps) * 10]
            g = T.Experimental.PatternSignalGenerator(vt, seq, repeat=False, start_delay=0, spacing=0)
            gens.append(g)
            vmap[c] = g.getChannel()
        if delays:
            for c, d in delays.items():
                vt.setInputDelay(vmap[c], int(d))
        n_total = sum(len(t) for t in tags.values()) + 1000
        stream = T.TimeTagStream(vt, n_total, list(vmap.values()))
        objs = build(vt, lambda n: vmap[n] if n >= 0 else -vmap[-n])
        vt.run(1.0)
        if after_run is not None:
            after_run(objs)
        # Real-time simulation: wait until the stream has passed end_ps.
        deadline = time.monotonic() + end_ps * 1e-12 * 3 + 5.0
        chunks = []
        t_now = 0
        while time.monotonic() < deadline:
            time.sleep(0.02)
            b = stream.getData()
            chunks.append((b.getTimestamps().copy(), b.getChannels().copy(), b.getEventTypes().copy()))
            t_now = b.tGetData
            if t_now > end_ps:
                break
        self.stream_end_ps = t_now
        self.results = extract(objs)
        stream.stop()
        inv = {v: k for k, v in vmap.items()}
        ts = np.concatenate([c[0] for c in chunks]) if chunks else np.zeros(0, np.int64)
        chs = np.concatenate([c[1] for c in chunks]) if chunks else np.zeros(0, np.int32)
        types = np.concatenate([c[2] for c in chunks]) if chunks else np.zeros(0, np.uint8)
        keep = types == 0
        self.times = ts[keep]
        self.channels = np.array([inv.get(int(c), 0) for c in chs[keep]], dtype=np.int32)
        del objs, stream, gens
        T.freeTimeTagger(vt)
