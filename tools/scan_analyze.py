#!/usr/bin/env python3
"""Read a scan recording (console K then S, docs/bench.md) out of a log.

Prints the scan rate, each channel's level and the passes it finds: a channel rising
well above its own resting level, timed at the centroid of the peak. A pass also seen on
another channel at the same moment goes to the one with the highest level, and the
others are listed. Like the detector's relative re-arm (--rearm, 16 dB by default as
CONFIG_CHECKPOINT_REARM_DROP_DB, 0 = off), a drone that stays well up between passes
still gives one pass per lap: a rise is split where its 1 s average fell that far under
its highest level and rose that far again.

Two dump formats: the sdr_rx engine's (format=qdb: each row the mean of decim rotations,
levels in 0.25 dB codes, dBFS x 4, -32768 for no reading) and the older C5VRX one (one
sweep per row, IQ power x256, 0xffff for no reading). A format=qdb row may end with a
4-hex-digit word, one digit per hop in reading order: that LO's captures that hit an ADC
rail among the row's rotations (15 at most). Rows without it are read as before; with it,
each pass also gets the saturated share of every LO over its span.

usage: tools/scan_analyze.py [run.log] [--margin DB] [--alpha A] [--rearm DB]
"""
import argparse
import math
import re
import statistics
from pathlib import Path

CHANNELS = 8
ROW = re.compile(r"SCAN,(\d+),((?:-?\d+,){7}-?\d+)(?:,([0-9a-fA-F]{4}))?\s*$")
SAT_HOPS = 4            # the saturated word: one 4-bit count per hop, hop 0 first
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
REARM_DB = 16.0         # --rearm default: CONFIG_CHECKPOINT_REARM_DROP_DB's
REARM_TAU_S = 1.0       # the relative re-arm's average (PD_REARM_TAU_MS)
REFRACTORY_S = 1.5      # sd_config_default's refractory_ms


def read_sat(word):
    """The saturated word's counts per hop, hop 0 first; None for a row without one."""
    if word is None:
        return None
    v = int(word, 16)
    return [(v >> (4 * (SAT_HOPS - 1 - j))) & 0xf for j in range(SAT_HOPS)]


def read_scans(text, sat=False):
    """The rows of the last complete dump, [(t_us, [value or None x 8])], and its header: the
    dump's begin line (when it arrived; the end line repeats what the analysis needs but rot_us)
    and its end line. sat: each row is (t_us, values, [saturated captures per hop] or None)."""
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
                values = [None if int(v) in (NO_DATA, QDB_NONE) else int(v) for v in m[2].split(",")]
                rows.append((int(m[1]), values, read_sat(m[3])) if sat else (int(m[1]), values))
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


def read_groups(header):
    """The hops in reading order, each the channels (0..7) of its capture, from the dump's order=
    (1:5:2.. one channel a hop; 1+2:3+4.. channels sharing one capture; default R1 first, one each)."""
    m = re.search(r"order=([\d:+]+)", header)
    return [[int(c) - 1 for c in g.split("+")] for g in m[1].split(":")] if m else [[c] for c in range(CHANNELS)]


def read_order(header):
    """Each of R1..R8's group (hop) in a rotation, from the dump's order= (read_groups)."""
    groups = read_groups(header)
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
    return [[(r[0] + offset[ch]) / 1e6 for r in rows] for ch in range(CHANNELS)], period


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


def split_rearm(times, levels, a, b, drop):
    """The detector's relative re-arm inside the segment [a, b]: once REFRACTORY_S after its highest
    level (about the detector's refractory period from its ring) the level's REARM_TAU_S average
    stands drop dB under that level, the next pass starts after the lowest of that average, as soon
    as the level rises drop dB over it. [(first, last), ...]; drop 0: the segment whole."""
    if drop <= 0:
        return [(a, b)]
    out, start = [], a
    slow = prev = ref = ref_t = trough = trough_i = None
    for i in range(a, b + 1):
        v = levels[i]
        if v is None:
            continue
        t = times[i]
        slow = v if slow is None else slow + min(1.0, (t - prev) / REARM_TAU_S) * (v - slow)
        prev = t
        if trough is None:
            if ref is None or v > ref:
                ref, ref_t = v, t
            if t - ref_t >= REFRACTORY_S and slow <= ref - drop:
                trough, trough_i = slow, i
        else:
            if slow < trough:
                trough, trough_i = slow, i
            if v > trough + drop:
                out.append((start, trough_i))
                start, ref, ref_t, trough = trough_i + 1, v, t, None
    out.append((start, b))
    return out


def find_passes(rows, margin, alpha, position=range(CHANNELS), dump=None, rearm=0.0):
    """alpha: smoothing weight per REF_US (17.5 ms). dump: read_dump()'s, None for legacy rows.
    rearm: split a long rise as the detector's relative re-arm does (split_rearm), 0 = off."""
    qdb = bool(dump and dump["qdb"])
    times, period = channel_times(rows, list(position), dump)
    raw = [[None if r[1][ch] is None else to_db(r[1][ch], qdb) for r in rows] for ch in range(CHANNELS)]
    floors = [rest_level([v for v in raw[ch] if v is not None]) for ch in range(CHANNELS)]
    a_row = row_alpha(alpha, period)
    passes = []
    for ch in range(CHANNELS):
        if floors[ch] is None:
            continue
        levels = smooth(raw[ch], a_row)
        threshold = floors[ch] + margin if qdb else max(floors[ch] + margin, SIGNAL_DB)
        for a, b in (sub for seg in segments(times[ch], levels, threshold)
                     for sub in split_rearm(times[ch], levels, seg[0], seg[1], rearm)):
            peak = max(levels[i] for i in range(a, b + 1) if levels[i] is not None)
            top = [i for i in range(a, b + 1) if levels[i] is not None and levels[i] >= peak - PEAK_BAND_DB]
            weights = [10 ** (levels[i] / 10) for i in top]
            at = sum(times[ch][i] * w for i, w in zip(top, weights)) / sum(weights)
            passes.append({"ch": ch, "t": at, "peak": peak, "over": peak - floors[ch],
                           "start": times[ch][a], "end": times[ch][b], "rows": (a, b), "bleed": []})
    passes.sort(key=lambda p: -p["peak"])
    kept = []
    for p in passes:
        owner = next((k for k in kept if p["start"] <= k["end"] and k["start"] <= p["end"]), None)
        if owner:
            owner["bleed"].append(f"R{p['ch'] + 1}({p['peak'] - owner['peak']:+.0f} dB)")
        else:
            kept.append(p)
    return sorted(kept, key=lambda p: p["t"]), floors, raw, period


def saturated_share(rows, span, decim):
    """Per hop, the share (0..1) of its captures that hit an ADC rail over the rows span = (first,
    last) of read_scans(text, sat=True); None when those rows carry no saturated word. A count
    reads 15 at most, so with decim over 15 a fully saturated row reads 15 / decim."""
    sats = [r[2] for r in rows[span[0]:span[1] + 1] if len(r) > 2 and r[2] is not None]
    if not sats:
        return None
    return [min(1.0, sum(s[j] for s in sats) / (len(sats) * decim)) for j in range(SAT_HOPS)]


def sat_text(share, groups):
    """'saturated 1+2 0% 3+4 0% 5+6 41% 7+8 0%': each hop by its channels (order=)."""
    names = ["+".join(str(c + 1) for c in groups[j]) if j < len(groups) else f"hop{j}" for j in range(SAT_HOPS)]
    return "saturated " + " ".join(f"{n} {v * 100:.0f}%" for n, v in zip(names, share))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", nargs="?", default="run.log", type=Path)
    ap.add_argument("--margin", type=float, default=8.0, help="dB above a channel's resting level that counts (default 8)")
    ap.add_argument("--alpha", type=float, default=0.25,
                    help="smoothing weight per 17.5 ms, as the detector's alpha (default 0.25: tau 61 ms)")
    ap.add_argument("--rearm", type=float, default=REARM_DB,
                    help="split a rise as the detector's relative re-arm does, dB (default 16, the firmware's; 0 = off)")
    a = ap.parse_args()
    rows, header = read_scans(a.log.read_text(errors="replace"), sat=True)
    if len(rows) < 10:
        raise SystemExit(f"no scan recording in {a.log}: K then S, passes, K then S again")
    dump = read_dump(header, rows)
    passes, floors, raw, period = find_passes(rows, a.margin, a.alpha, read_order(header), dump, a.rearm)
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
    print(f"{len(passes)} pass(es), margin {a.margin:g} dB, re-arm {a.rearm:g} dB:")
    groups = read_groups(header)
    for p in passes:
        share = saturated_share(rows, p["rows"], dump["decim"]) if dump["qdb"] else None
        print(f"  t={p['t']:7.3f} s  R{p['ch'] + 1}  peak {p['peak']:5.1f} {unit}  +{p['over']:.1f} dB over its rest  "
              f"lasting {(p['end'] - p['start']) * 1000:.0f} ms" + (f"  also on {' '.join(p['bleed'])}" if p["bleed"] else "")
              + (f"  {sat_text(share, groups)}" if share is not None else ""))


if __name__ == "__main__":
    main()
