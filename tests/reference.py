"""Brute-force reference implementations of the Dantagger measurements.

Slow and obviously correct; they encode the Swabian semantics established with
the Swabian library itself (tests/oracle, GAMEPLAN.md section "Conventions").
Input: a merged tag stream (times, channels) in stream order.
"""

import numpy as np


def merge(per_channel, delays=None, deadtimes=None):
    """Per-channel raw times -> merged stream: dead time (non-extending, on raw
    times), then delay, then order by (time, channel)."""
    times, chans = [], []
    for c, raw in per_channel.items():
        raw = np.sort(np.asarray(raw, dtype=np.int64))
        dead = (deadtimes or {}).get(c, 0)
        if dead > 0 and len(raw):
            keep, last = [], None
            for t in raw:
                if last is None or t - last >= dead:
                    keep.append(t)
                    last = t
            raw = np.array(keep, dtype=np.int64)
        times.append(raw + (delays or {}).get(c, 0))
        chans.append(np.full(len(raw), c, dtype=np.int8))
    t = np.concatenate(times) if times else np.zeros(0, np.int64)
    ch = np.concatenate(chans) if chans else np.zeros(0, np.int8)
    order = np.lexsort((ch, t))
    return t[order], ch[order]


def countrate(t, ch, channels, duration):
    counts = np.array([np.sum(ch == c) for c in channels], dtype=np.int64)
    return counts / (duration * 1e-12), counts


def counter(t, ch, channels, binwidth, n_values, capture):
    """Completed bins of capture time [0, capture); rolling=True layout."""
    completed = capture // binwidth
    m = min(completed, n_values)
    out = np.zeros((len(channels), n_values), dtype=np.int64)
    totals = np.zeros(len(channels), dtype=np.int64)
    for s, c in enumerate(channels):
        tt = t[ch == c]
        tt = tt[tt < completed * binwidth]
        totals[s] = len(tt)
        bins = np.bincount(tt // binwidth, minlength=completed)[:completed] if completed else np.zeros(0)
        if m:
            out[s, n_values - m:] = bins[completed - m:]
    return out, totals


def correlation(t, ch, c1, c2, binwidth, n_bins):
    origin = -((binwidth * n_bins) // 2)   # Swabian: -(n*bw)/2 truncated toward zero
    hist = np.zeros(n_bins, dtype=np.int64)
    a = t[ch == c1]
    b = np.sort(t[ch == c2])
    lo, hi = origin, origin + binwidth * n_bins
    for x in a:   # d = x - y in [lo, hi)  <=>  y in (x - hi, x - lo]
        i0 = np.searchsorted(b, x - hi, side="right")
        i1 = np.searchsorted(b, x - lo, side="right")
        d = x - b[i0:i1]
        np.add.at(hist, (d - origin) // binwidth, 1)
    return hist, origin + np.arange(n_bins, dtype=np.int64) * binwidth


def histogram(t, ch, click, start, binwidth, n_bins):
    """Every start at or before the click in stream order (the click itself when click == start)."""
    rng = binwidth * n_bins
    hist = np.zeros(n_bins, dtype=np.int64)
    pos = np.arange(len(t))
    sp = pos[ch == start]
    st = t[ch == start]
    for i in pos[ch == click]:
        k = np.searchsorted(sp, i, side="right")   # starts with stream position <= i
        d = t[i] - st[:k]
        d = d[(d >= 0) & (d < rng)]
        np.add.at(hist, d // binwidth, 1)
    return hist, np.arange(n_bins, dtype=np.int64) * binwidth


def histogram_nd(t, ch, start, stops, binwidths, n_bins):
    hist = np.zeros(int(np.prod(n_bins)), dtype=np.int64)
    strides = [int(np.prod(n_bins[k + 1:])) for k in range(len(stops))]
    active, s0, got, flat = False, 0, set(), 0
    for tt, c in zip(t.tolist(), ch.tolist()):
        if c == start:
            active, s0, got, flat = True, tt, set(), 0
        if not active:
            continue
        for k, sc in enumerate(stops):
            if sc != c or k in got:
                continue
            d = tt - s0
            if d >= binwidths[k] * n_bins[k]:
                active = False
                break
            flat += (d // binwidths[k]) * strides[k]
            got.add(k)
        if active and len(got) == len(stops):
            hist[flat] += 1
            active = False
    return hist



def coincidence_at(t, ch, channels, window):
    """Swabian Coincidence (CoincidenceTimestamp.Last): the stream positions
    of the tags on a channel of the group (the set of `channels`) for which
    every other group channel has an earlier tag in the stream less than
    `window` before it.  Each tag counts once, nothing is used up."""
    group = list(dict.fromkeys(int(c) for c in channels))
    idx = np.arange(len(t))
    pos = {c: idx[ch == c] for c in group}
    out = []
    for i in idx[np.isin(ch, group)]:
        if window <= 0:
            continue
        ok = True
        for j in group:
            if j == ch[i]:
                continue
            k = np.searchsorted(pos[j], i) - 1   # the latest tag of j before tag i
            if k < 0 or t[i] - t[pos[j][k]] >= window:
                ok = False
                break
        if ok:
            out.append(i)
    return np.array(out, dtype=np.int64)


def coincidence(t, ch, channels, window):
    """The times of the coincidence tags (see coincidence_at)."""
    return t[coincidence_at(t, ch, channels, window)]


def insert_after(t, ch, positions, channel):
    """The stream with a tag on `channel` right after each tag at `positions`
    (same time): where Swabian puts a virtual channel's tags."""
    t2 = np.insert(t, np.asarray(positions, dtype=np.int64) + 1, t[positions])
    c2 = np.insert(ch, np.asarray(positions, dtype=np.int64) + 1, channel)
    return t2, c2
