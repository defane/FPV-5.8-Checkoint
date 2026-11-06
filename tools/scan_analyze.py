#!/usr/bin/env python3
"""Read a scan prototype recording (console K then S, docs/bench.md) out of a log.

Prints the scan rate, each channel's level and the passes it finds: a channel rising
well above its own resting level, timed at the centroid of the peak. A pass also seen on
another channel at the same moment goes to the one with the highest level, and the
others are listed.

usage: tools/scan_analyze.py [run.log] [--margin DB] [--alpha A]
"""
import argparse
import math
import re
import statistics
from pathlib import Path

CHANNELS = 8
NO_DATA = 0xffff
FLOOR_DB = -30.0        # power 0 after the DC removal
SIGNAL_DB = -5.0        # below this it is quantisation noise: a channel whose median is
                        # the floor would otherwise "rise" 20 dB on a single flickering bit
GAP_S = 0.3             # dips shorter than this stay in the same pass
MIN_S = 0.05            # shorter than this is a glitch
PEAK_BAND_DB = 3.0      # the centroid covers what is within this of the peak
REST_QUANTILE = 0.2     # a channel's resting level: a VTX held close for half a recording
                        # would lift the median itself


def read_scans(text):
    """The rows of the last complete dump, [(t_us, [power or None x 8])], and its end line."""
    rows, header, done = [], "", []
    for line in text.splitlines():
        if "SCANDUMP begin" in line:
            rows = []
        elif "SCANDUMP end" in line:
            header = line[line.index("SCANDUMP end"):].strip()
            done = (rows, header)
            rows = []
        else:
            m = re.search(r"SCAN,(\d+),((?:\d+,){7}\d+)\s*$", line)
            if m:
                rows.append((int(m[1]), [None if int(p) == NO_DATA else int(p) for p in m[2].split(",")]))
    return done or (rows, header)


def to_db(power):
    return FLOOR_DB if not power else max(FLOOR_DB, 10 * math.log10(power / 256))


def read_order(header):
    """The position in a sweep of R1..R8, from the dump's order=1:5:2.. (default R1 first)."""
    m = re.search(r"order=([\d:]+)", header)
    seq = [int(c) - 1 for c in m[1].split(":")] if m else list(range(CHANNELS))
    return [seq.index(ch) for ch in range(CHANNELS)]


def channel_times(rows, position):
    """Each sample's own time: the channels of a sweep are read one after the other."""
    period = statistics.median(b[0] - a[0] for a, b in zip(rows, rows[1:]))
    return [[(t + (position[ch] + 0.5) / CHANNELS * period) / 1e6 for t, _ in rows] for ch in range(CHANNELS)], period


def rest_level(values):
    return sorted(values)[int(len(values) * REST_QUANTILE)]


def smooth(values, alpha):
    out, level = [], None
    for v in values:
        if v is not None:
            level = v if level is None else level + alpha * (v - level)
        out.append(level)
    return out


def segments(times, levels, threshold):
    """(first, last) index ranges above threshold, short gaps closed, short bursts dropped."""
    found = []
    for i, v in enumerate(levels):
        if v is None or v < threshold:
            continue
        if found and times[i] - times[found[-1][1]] <= GAP_S:
            found[-1][1] = i
        else:
            found.append([i, i])
    return [(a, b) for a, b in found if times[b] - times[a] >= MIN_S]


def find_passes(rows, margin, alpha, position=range(CHANNELS)):
    times, period = channel_times(rows, list(position))
    raw = [[None if p[ch] is None else to_db(p[ch]) for _, p in rows] for ch in range(CHANNELS)]
    floors = [rest_level([v for v in raw[ch] if v is not None]) for ch in range(CHANNELS)]
    passes = []
    for ch in range(CHANNELS):
        levels = smooth(raw[ch], alpha)
        for a, b in segments(times[ch], levels, max(floors[ch] + margin, SIGNAL_DB)):
            peak = max(levels[i] for i in range(a, b + 1) if levels[i] is not None)
            top = [i for i in range(a, b + 1) if levels[i] is not None and levels[i] >= peak - PEAK_BAND_DB]
            weights = [10 ** (levels[i] / 10) for i in top]
            at = sum(times[ch][i] * w for i, w in zip(top, weights)) / sum(weights)
            passes.append({"ch": ch, "t": at, "peak": peak, "over": peak - floors[ch],
                           "start": times[ch][a], "end": times[ch][b], "bleed": []})
    passes.sort(key=lambda p: -p["peak"])
    kept = []
    for p in passes:
        owner = next((k for k in kept if p["start"] <= k["end"] and k["start"] <= p["end"]), None)
        if owner:
            owner["bleed"].append(f"R{p['ch'] + 1}({p['peak'] - owner['peak']:+.0f} dB)")
        else:
            kept.append(p)
    return sorted(kept, key=lambda p: p["t"]), floors, raw, period


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", nargs="?", default="run.log", type=Path)
    ap.add_argument("--margin", type=float, default=8.0, help="dB above a channel's resting level that counts (default 8)")
    ap.add_argument("--alpha", type=float, default=0.25, help="smoothing per sweep (default 0.25)")
    a = ap.parse_args()
    rows, header = read_scans(a.log.read_text(errors="replace"))
    if len(rows) < 10:
        raise SystemExit(f"no scan recording in {a.log}: K then S, passes, K then S again")
    passes, floors, raw, period = find_passes(rows, a.margin, a.alpha, read_order(header))
    gaps = sorted(y[0] - x[0] for x, y in zip(rows, rows[1:]))
    print(header)
    print(f"{len(rows)} sweeps over {(rows[-1][0] - rows[0][0]) / 1e6:.1f} s: {1e6 / period:.1f} Hz per channel, "
          f"sweep {gaps[len(gaps) // 2] / 1000:.1f} ms median, {gaps[-1] / 1000:.1f} ms worst")
    for ch in range(CHANNELS):
        v = [x for x in raw[ch] if x is not None]
        print(f"R{ch + 1}: rest {floors[ch]:6.1f} dB  median {statistics.median(v):6.1f} dB  "
              f"peak {max(v):6.1f} dB  missing {len(rows) - len(v)}")
    print(f"{len(passes)} pass(es), margin {a.margin:g} dB:")
    for p in passes:
        print(f"  t={p['t']:7.3f} s  R{p['ch'] + 1}  peak {p['peak']:5.1f} dB  +{p['over']:.1f} dB over its rest  "
              f"lasting {(p['end'] - p['start']) * 1000:.0f} ms" + (f"  also on {' '.join(p['bleed'])}" if p["bleed"] else ""))


if __name__ == "__main__":
    main()
