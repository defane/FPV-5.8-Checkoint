#!/usr/bin/env python3
"""Read a scan recording (console K then S, docs/bench.md) out of a log.

Prints the scan rate, each channel's level and the passes it finds: a channel rising
well above its own resting level, timed at the centroid of the peak. A pass also seen on
another channel at the same moment goes to the one with the highest level, and the
others are listed.

Two dump formats: the sdr_rx engine's (format=qdb: each row the mean of decim rotations,
levels in 0.25 dB codes, dBFS x 4, -32768 for no reading) and the older C5VRX one (one
sweep per row, IQ power x256, 0xffff for no reading).

usage: tools/scan_analyze.py [run.log] [--margin DB] [--alpha A]
"""
import argparse
import math
import re
import statistics
from pathlib import Path

CHANNELS = 8
ROW = re.compile(r"SCAN,(\d+),((?:-?\d+,){7}-?\d+)\s*$")
NO_DATA = 0xffff        # legacy power format
QDB_NONE = -32768       # format=qdb
FLOOR_DB = -30.0        # legacy: power 0 after the DC removal
SIGNAL_DB = -5.0        # legacy: below this it is quantisation noise: a channel whose median is
                        # the floor would otherwise "rise" 20 dB on a single flickering bit
REF_US = 17500          # --alpha is the smoothing weight per this interval (the detector's alpha)
GAP_S = 0.3             # dips shorter than this stay in the same pass
MIN_S = 0.05            # shorter than this is a glitch
PEAK_BAND_DB = 3.0      # the centroid covers what is within this of the peak
REST_QUANTILE = 0.2     # a channel's resting level: a VTX held close for half a recording
                        # would lift the median itself


def read_scans(text):
    """The rows of the last complete dump, [(t_us, [value or None x 8])], and its header: the
    dump's begin line (when it arrived; the end line repeats what the analysis needs but rot_us)
    and its end line."""
    rows, begin, done = [], "", []
    for line in text.splitlines():
        if "SCANDUMP begin" in line:
            rows, begin = [], line[line.index("SCANDUMP begin"):].strip()
        elif "SCANDUMP end" in line:
            end = line[line.index("SCANDUMP end"):].strip()
            done = (rows, f"{begin}\n{end}" if begin else end)
            rows, begin = [], ""
        else:
            m = ROW.search(line)
            if m:
                rows.append((int(m[1]), [None if int(v) in (NO_DATA, QDB_NONE) else int(v) for v in m[2].split(",")]))
    return done or (rows, begin)


def field(header, name):
    m = re.search(rf"\b{name}=(\S+)", header)
    return m[1] if m else None


def read_dump(header, rows):
    """What the header says of its rows: the format, the rotations per row (decim) and the
    rotation time (rot_us, from the begin line, else the rows' median period over decim)."""
    qdb = field(header, "format") == "qdb"
    decim = int(field(header, "decim") or 1) if qdb else 1
    rot = field(header, "rot_us") if qdb else None
    if rot:
        rot_us = float(rot)
    else:
        rot_us = statistics.median(b[0] - a[0] for a, b in zip(rows, rows[1:])) / decim
    return {"qdb": qdb, "decim": decim, "rot_us": rot_us}


def to_db(value, qdb=False):
    if qdb:
        return value / 4
    return FLOOR_DB if not value else max(FLOOR_DB, 10 * math.log10(value / 256))


def read_order(header):
    """Each of R1..R8's group in a rotation, from the dump's order= (1:5:2.. one channel a hop;
    1+2:3+4.. channels sharing one capture; default R1 first, one each)."""
    m = re.search(r"order=([\d:+]+)", header)
    groups = [[int(c) - 1 for c in g.split("+")] for g in m[1].split(":")] if m else [[c] for c in range(CHANNELS)]
    return [next(i for i, g in enumerate(groups) if ch in g) for ch in range(CHANNELS)]


def channel_times(rows, position, dump=None):
    """Each sample's own time (s) and the row period (us). A legacy row is one sweep, its
    channels read one group after the other: group g of G at (g + 0.5) / G of the period. A qdb
    row is the mean of decim rotations of rot_us from its time: group g of G is centred at
    period / 2 + ((g + 0.5) / G - 0.5) * rot_us, with period = decim * rot_us."""
    groups = max(position) + 1
    if dump and dump["qdb"]:
        rot, period = dump["rot_us"], dump["decim"] * dump["rot_us"]
        offset = [period / 2 + ((position[ch] + 0.5) / groups - 0.5) * rot for ch in range(CHANNELS)]
    else:
        period = statistics.median(b[0] - a[0] for a, b in zip(rows, rows[1:]))
        offset = [(position[ch] + 0.5) / groups * period for ch in range(CHANNELS)]
    return [[(t + offset[ch]) / 1e6 for t, _ in rows] for ch in range(CHANNELS)], period


def row_alpha(alpha, period_us):
    """The weight per row of a smoothing of alpha per REF_US: the same time constant at any row rate."""
    return 1 - (1 - alpha) ** (period_us / REF_US)


def rest_level(values):
    return sorted(values)[int(len(values) * REST_QUANTILE)] if values else None


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


def find_passes(rows, margin, alpha, position=range(CHANNELS), dump=None):
    """alpha: smoothing weight per REF_US (17.5 ms). dump: read_dump()'s, None for legacy rows."""
    qdb = bool(dump and dump["qdb"])
    times, period = channel_times(rows, list(position), dump)
    raw = [[None if p[ch] is None else to_db(p[ch], qdb) for _, p in rows] for ch in range(CHANNELS)]
    floors = [rest_level([v for v in raw[ch] if v is not None]) for ch in range(CHANNELS)]
    a_row = row_alpha(alpha, period)
    passes = []
    for ch in range(CHANNELS):
        if floors[ch] is None:
            continue
        levels = smooth(raw[ch], a_row)
        threshold = floors[ch] + margin if qdb else max(floors[ch] + margin, SIGNAL_DB)
        for a, b in segments(times[ch], levels, threshold):
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
    ap.add_argument("--alpha", type=float, default=0.25,
                    help="smoothing weight per 17.5 ms, as the detector's alpha (default 0.25: tau 61 ms)")
    a = ap.parse_args()
    rows, header = read_scans(a.log.read_text(errors="replace"))
    if len(rows) < 10:
        raise SystemExit(f"no scan recording in {a.log}: K then S, passes, K then S again")
    dump = read_dump(header, rows)
    passes, floors, raw, period = find_passes(rows, a.margin, a.alpha, read_order(header), dump)
    gaps = sorted(y[0] - x[0] for x, y in zip(rows, rows[1:]))
    span = (rows[-1][0] - rows[0][0]) / 1e6
    unit = "dBFS" if dump["qdb"] else "dB"
    print(header)
    if dump["qdb"]:
        print(f"{len(rows)} rows of {dump['decim']} rotation(s) over {span:.1f} s: {1e6 / dump['rot_us']:.1f} Hz "
              f"per channel (rotation {dump['rot_us'] / 1000:.2f} ms), row {gaps[len(gaps) // 2] / 1000:.1f} ms "
              f"median, {gaps[-1] / 1000:.1f} ms worst")
    else:
        print(f"{len(rows)} sweeps over {span:.1f} s: {1e6 / period:.1f} Hz per channel, "
              f"sweep {gaps[len(gaps) // 2] / 1000:.1f} ms median, {gaps[-1] / 1000:.1f} ms worst")
    for ch in range(CHANNELS):
        v = [x for x in raw[ch] if x is not None]
        if not v:
            print(f"R{ch + 1}: no reading")
            continue
        print(f"R{ch + 1}: rest {floors[ch]:6.1f} {unit}  median {statistics.median(v):6.1f} {unit}  "
              f"peak {max(v):6.1f} {unit}  missing {len(rows) - len(v)}")
    print(f"{len(passes)} pass(es), margin {a.margin:g} dB:")
    for p in passes:
        print(f"  t={p['t']:7.3f} s  R{p['ch'] + 1}  peak {p['peak']:5.1f} {unit}  +{p['over']:.1f} dB over its rest  "
              f"lasting {(p['end'] - p['start']) * 1000:.0f} ms" + (f"  also on {' '.join(p['bleed'])}" if p["bleed"] else ""))


if __name__ == "__main__":
    main()
