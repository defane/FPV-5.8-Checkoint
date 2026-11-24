#!/usr/bin/env python3
"""tools/scan_analyze.py on synthetic recordings: passes, bleed, steady interference, the
saturated-capture word of the newer rows."""
import contextlib
import importlib.util
import io
import math
import random
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("scan_analyze", ROOT / "tools/scan_analyze.py")
sa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sa)

PERIOD_US = 11400


def power(db):
    return int(256 * 10 ** (db / 10))


def recording(events, steady=None, seconds=12, drop=0.0):
    """events: (channel, t_s, peak_db, half_width_s)."""
    rnd = random.Random(7)
    lines = ["HB t_s=5 stage=ready", "SCANDUMP begin sweeps=9 kept=9 no_data=0 gain=52"]
    for k in range(int(seconds * 1e6 / PERIOD_US)):
        t_us = k * PERIOD_US
        row = []
        for ch in range(8):
            db = (steady or {}).get(ch, -10.0) + rnd.gauss(0, 1.5)
            t = (t_us + (ch + 0.5) / 8 * PERIOD_US) / 1e6
            for c, at, peak, half in events:
                if c == ch and abs(t - at) < 3 * half:
                    db = max(db, peak - ((t - at) / half) ** 2 * 6 + rnd.gauss(0, 1.5))
            row.append(0xffff if rnd.random() < drop else power(db))
        lines.append("SCAN," + ",".join(str(v) for v in [t_us] + row))
        if k % 200 == 0:
            lines.append("STATUS channel=A3")          # other tasks print in between
    lines.append("SCANDUMP end")
    return "\n".join(lines)


ROT_US = 2000           # an sdr_rx rotation: four LOs, two channels a capture (1+2 first)
ORDER = "1+2:3+4:5+6:7+8"


def sat_word(r0, decim, sat):
    """The row's saturated word: per hop, its captures within one of sat's (hop, t0_s, t1_s), 15 at
    most, hop 0 in the top digit (lt_bench.c)."""
    word = 0
    for hop in range(4):
        n = sum(1 for r in range(r0, r0 + decim) for h, t0, t1 in sat
                if h == hop and t0 <= (r * ROT_US + (hop + 0.5) / 4 * ROT_US) / 1e6 < t1)
        word |= min(n, 15) << (4 * (3 - hop))
    return f"{word:04x}"


def qdb_recording(events, decim, seconds=12, floor_db=-60.0, sd_db=1.2, drop=0.0, begin=True, sat=None):
    """The sdr_rx engine's dump (K then S, format=qdb): every channel read once a rotation, at the
    middle of its pair's quarter, sd_db of noise per reading; a row is the mean of decim rotations
    in 0.25 dB codes, timed at its first rotation's start. events: (channel, t_s, dB over the
    floor, half_width_s). sat None: the older rows, 8 levels; else [(hop, t0_s, t1_s)], the
    captures of that hop that hit a rail, and every row ends with its saturated word."""
    rnd = random.Random(7)
    rotations = int(seconds * 1e6 / ROT_US)
    rows = []
    for r0 in range(0, rotations - decim + 1, decim):
        sums, n = [0.0] * 8, [0] * 8
        for r in range(r0, r0 + decim):
            for ch in range(8):
                if rnd.random() < drop:
                    continue
                t = (r * ROT_US + (ch // 2 + 0.5) / 4 * ROT_US) / 1e6
                db = floor_db + rnd.gauss(0, sd_db)
                for c, at, over, half in events:
                    if c == ch and abs(t - at) < 3 * half:
                        db = max(db, floor_db + over - ((t - at) / half) ** 2 * 6 + rnd.gauss(0, sd_db))
                sums[ch] += 4 * db
                n[ch] += 1
        rows.append("SCAN," + ",".join(str(v) for v in [r0 * ROT_US] + [round(s / k) if k else -32768
                                                                          for s, k in zip(sums, n)])
                    + ("" if sat is None else "," + sat_word(r0, decim, sat)))
    lines = ["HB t_s=5 stage=ready"]
    if begin:
        lines.append(f"SCANDUMP begin rotations={rotations} kept={len(rows)} kept_s={seconds} decim={decim} "
                     f"format=qdb order={ORDER} gain=40 rot_us={ROT_US} fw=test")
    lines += rows
    lines.append(f"SCANDUMP end rotations={rotations} decim={decim} format=qdb order={ORDER} gain=40")
    return "\n".join(lines)


def analyze_qdb(text, margin=10.0, alpha=0.25):
    rows, header = sa.read_scans(text)
    return sa.find_passes(rows, margin, alpha, sa.read_order(header), sa.read_dump(header, rows))[0]


class AnalyzeQdb(unittest.TestCase):
    EVENTS = [(5, 3.0, 25, 0.15), (5, 7.0, 22, 0.15), (2, 10.0, 24, 0.15)]

    def test_2_ms_and_28_ms_rows_find_the_same_passes(self):
        fast = analyze_qdb(qdb_recording(self.EVENTS, 1))
        slow = analyze_qdb(qdb_recording(self.EVENTS, 14))
        self.assertEqual([p["ch"] for p in fast], [5, 5, 2])
        self.assertEqual([p["ch"] for p in slow], [5, 5, 2])
        for a, b in zip(fast, slow):
            self.assertLess(abs(a["t"] - b["t"]), 0.03, "the row rate does not move a pass")
        for p, at in zip(fast + slow, (3.0, 7.0, 10.0) * 2):
            self.assertLess(abs(p["t"] - at), 0.1, "the 61 ms smoothing delay, no more")

    def test_nothing_on_noise(self):
        self.assertEqual(analyze_qdb(qdb_recording([], 1)), [])
        self.assertEqual(analyze_qdb(qdb_recording([], 14)), [])

    def test_relative_rearm_splits_a_vtx_staying_up(self):
        # A VTX 28 dB up from 3 to 27 s (the bench's one 22.5 s pass), with passes 20 dB over it at 6,
        # 14 and 22 s: one long rise without the re-arm, one pass per lap with it, as the detector.
        events = [(5, 3.0 + k / 4, 28, 0.5) for k in range(97)] + [(5, at, 48, 0.3) for at in (6.0, 14.0, 22.0)]
        text = qdb_recording(events, 14, seconds=36)
        rows, header = sa.read_scans(text)
        args = (rows, 10.0, 0.25, sa.read_order(header), sa.read_dump(header, rows))
        whole = sa.find_passes(*args)[0]
        self.assertEqual([p["ch"] for p in whole], [5], "rearm 0: the old segments")
        self.assertGreater(whole[0]["end"] - whole[0]["start"], 20.0)
        split = sa.find_passes(*args, rearm=16.0)[0]
        self.assertEqual([p["ch"] for p in split], [5, 5, 5])
        for p, at in zip(split, (6.0, 14.0, 22.0)):
            self.assertLess(abs(p["t"] - at), 0.15)
        self.assertEqual(len(sa.find_passes(*args, rearm=25.0)[0]), 1, "a 20 dB rise does not split at 25 dB")
        self.assertEqual(sa.split_rearm([0.0, 1.0], [5.0, 6.0], 0, 1, 0.0), [(0, 1)])

    @staticmethod
    def split(f, seconds, drop=16.0, step=0.02):
        """split_rearm over a smoothed level f(t) sampled every step s: the pieces' (start, end) in s."""
        ts = [k * step for k in range(int(seconds / step) + 1)]
        lv = [f(t) for t in ts]
        return [(round(ts[a], 2), round(ts[b], 2)) for a, b in sa.split_rearm(ts, lv, 0, len(ts) - 1, drop)]

    def test_relative_rearm_keeps_the_detectors_guards(self):
        # Traces like test_pass_detector.c's (smoothed levels, dB over the rest): the pieces are the
        # passes the detector rings there (1.5 s refractory, 1 s average, a pass 16 dB over the trough).
        def tri(t, tc, peak, half):
            return peak * max(0.0, 1.0 - abs(t - tc) / half)

        def bump(t):    # a lap at 3 s, the drone staying 30 dB up, laps at 13 and 23 s, a 10 dB bump at 18 s
            lap = max(50 - 20 * abs(t - 3), 50 - 20 * abs(t - 13), 50 - 20 * abs(t - 23))
            return 0.0 if t < 2 else lap if t < 3 else max(30 + tri(t, 18, 10, 1.0), lap)

        def faded(t):   # a pass held 45 dB up, 20 dB fades of 150 ms at 6 s and 300 ms at 9 s
            if t < 2 or t > 12:
                return 0.0
            if t < 3.25:
                return 50 - 20 * abs(t - 3)
            return 25.0 if 6 <= t < 6.15 or 9 <= t < 9.3 else 45.0

        def quick(t):   # two passes 1 s apart on a 20 dB plateau: the refractory period holds the second
            return 0.0 if t < 1 else 20 + max(tri(t, 5.0, 30, 0.3), tri(t, 6.0, 31, 0.3))

        self.assertEqual(self.split(bump, 30), [(0.0, 12.0), (12.02, 22.0), (22.02, 30.0)],
                         "a lap per piece; a 10 dB bump after the re-arm (under its 16 dB) splits nothing")
        self.assertEqual(self.split(faded, 14), [(0.0, 14.0)], "fades shorter than the 1 s average split nothing")
        self.assertEqual(self.split(quick, 12), [(0.0, 12.0)], "no re-arm within the refractory period")

    def test_no_signal_floor_clamp(self):
        # A legacy recording clamps the threshold at SIGNAL_DB (-5 dB); a -35 dBFS peak is a pass here.
        passes = analyze_qdb(qdb_recording([(1, 6.0, 25, 0.15)], 14))
        self.assertEqual([p["ch"] for p in passes], [1])
        self.assertLess(passes[0]["peak"], sa.SIGNAL_DB)

    def test_codes_and_missing_readings(self):
        rows, header = sa.read_scans(qdb_recording(self.EVENTS, 14, drop=0.02))
        self.assertIn("format=qdb", header)
        self.assertEqual(sa.to_db(-240, qdb=True), -60.0)
        self.assertEqual(sa.to_db(-1, qdb=True), -0.25)
        rows, _ = sa.read_scans("SCANDUMP begin decim=1 format=qdb\nSCAN,0,-240,-32768,-1,4,-240,-240,-240,-240\n"
                                "SCANDUMP end decim=1 format=qdb")
        self.assertEqual(rows, [(0, [-240, None, -1, 4, -240, -240, -240, -240])])
        self.assertEqual([p["ch"] for p in analyze_qdb(qdb_recording(self.EVENTS, 14, drop=0.02))], [5, 5, 2])

    def test_pair_order(self):
        self.assertEqual(sa.read_order(f"SCANDUMP end order={ORDER}"), [0, 0, 1, 1, 2, 2, 3, 3])
        self.assertEqual(sa.read_order("order=7+8:1+2:3+4:5+6"), [1, 1, 2, 2, 3, 3, 0, 0])

    def test_row_times_within_the_rotation(self):
        rows = [(k * 14 * ROT_US, [-240] * 8) for k in range(3)]
        dump = {"qdb": True, "decim": 14, "rot_us": ROT_US}
        times, period = sa.channel_times(rows, sa.read_order(f"order={ORDER}"), dump)
        self.assertEqual(period, 14 * ROT_US)
        # A row's centre is 14 ms after its start; R1/R2 are read in the first quarter of each
        # rotation, 0.75 ms before a rotation's middle, R7/R8 0.75 ms after it.
        self.assertAlmostEqual(times[0][1], (28000 + 14000 - 750) / 1e6)
        self.assertAlmostEqual(times[1][1], times[0][1])
        self.assertAlmostEqual(times[7][1], (28000 + 14000 + 750) / 1e6)
        one = [(k * ROT_US, [-240] * 8) for k in range(3)]
        times, _ = sa.channel_times(one, sa.read_order(f"order={ORDER}"), {"qdb": True, "decim": 1, "rot_us": ROT_US})
        self.assertAlmostEqual(times[2][1], (2000 + 750) / 1e6, msg="R3: middle of the second quarter")

    def test_lost_begin_line(self):
        text = qdb_recording(self.EVENTS, 14, begin=False)
        rows, header = sa.read_scans(text)
        dump = sa.read_dump(header, rows)
        self.assertTrue(dump["qdb"])
        self.assertEqual(dump["decim"], 14)
        self.assertAlmostEqual(dump["rot_us"], ROT_US, msg="from the rows' period over decim")
        self.assertEqual([p["ch"] for p in analyze_qdb(text)], [5, 5, 2])

    def test_header_holds_both_lines(self):
        rows, header = sa.read_scans(qdb_recording([], 14))
        self.assertIn("SCANDUMP begin", header)
        self.assertIn("SCANDUMP end", header)
        self.assertEqual(sa.read_dump(header, rows), {"qdb": True, "decim": 14, "rot_us": float(ROT_US)})

    def test_alpha_is_per_17_5_ms(self):
        self.assertAlmostEqual(sa.row_alpha(0.25, 17500), 0.25)
        self.assertAlmostEqual(sa.row_alpha(0.25, 35000), 1 - 0.75 ** 2)
        tau = [-p / math.log(1 - sa.row_alpha(0.25, p)) for p in (2000, 28000)]
        self.assertAlmostEqual(tau[0], tau[1], msg="the same time constant at any row rate")

    def test_vtx_parked_most_of_the_recording_keeps_the_rest_low(self):
        # R6 sits +15 dB over the floor for the first 7.2 s of 12 (a drone held by the gate, 60 %),
        # then a real pass at 10.5 s: a median rest would be the parked level and miss it.
        lines = qdb_recording([(5, 10.5, 25, 0.15)], 1).splitlines()
        for i, line in enumerate(lines):
            if line.startswith("SCAN,"):
                f = line.split(",")
                if int(f[1]) < 7_200_000:
                    f[7] = str(int(f[7]) + 60)   # R6, +15 dB in 0.25 dB codes
                lines[i] = ",".join(f)
        passes = analyze_qdb("\n".join(lines))
        self.assertTrue(any(p["ch"] == 5 and abs(p["t"] - 10.5) < 0.15 for p in passes),
                        "the pass after the parked stretch is found")

    def test_truncated_dump_after_a_complete_one(self):
        # The console closed in the middle of a second dump: its rows have no end line, so the
        # complete dump before it is the one analysed.
        full = qdb_recording([(1, 3.0, 25, 0.15)], 14)
        cut = qdb_recording([(6, 3.0, 25, 0.15)], 14).splitlines()
        cut = "\n".join(line for line in cut[: len(cut) // 2] if "SCANDUMP end" not in line)
        self.assertEqual([p["ch"] for p in analyze_qdb(full + "\n" + cut)], [1])

    def test_legacy_dump_stays_legacy(self):
        rows, header = sa.read_scans(recording([]))
        self.assertEqual(sa.read_dump(header, rows), {"qdb": False, "decim": 1, "rot_us": PERIOD_US})


class SaturatedWord(unittest.TestCase):
    """The newer K S rows end with a 4-hex-digit word: per hop, its captures that hit an ADC rail."""
    EVENTS = AnalyzeQdb.EVENTS
    CLIP = [(2, 2.95, 3.05)]   # LO 5825 (R5+R6) clips for 100 ms at R6's first pass

    def test_word_is_read_and_optional(self):
        text = ("SCANDUMP begin decim=15 format=qdb\nSCAN,0,-240,-32768,-1,4,-240,-240,-240,-240,0f10\n"
                "SCAN,29500,-240,-240,-240,-240,-240,-240,-240,-240\nSCANDUMP end decim=15 format=qdb")
        rows, _ = sa.read_scans(text, sat=True)
        self.assertEqual(rows, [(0, [-240, None, -1, 4, -240, -240, -240, -240], [0, 15, 1, 0]),
                                (29500, [-240] * 8, None)])
        rows, _ = sa.read_scans(text)
        self.assertEqual(rows[0], (0, [-240, None, -1, 4, -240, -240, -240, -240]), "without sat: as an older row")
        self.assertEqual(len(rows), 2)

    def test_old_and_new_rows_find_the_same_passes(self):
        old = analyze_qdb(qdb_recording(self.EVENTS, 15))
        new = analyze_qdb(qdb_recording(self.EVENTS, 15, sat=self.CLIP))
        self.assertEqual([p["ch"] for p in new], [5, 5, 2])
        self.assertEqual([(p["ch"], p["t"]) for p in old], [(p["ch"], p["t"]) for p in new])

    def test_share_per_pass(self):
        rows, header = sa.read_scans(qdb_recording(self.EVENTS, 15, sat=self.CLIP), sat=True)
        dump = sa.read_dump(header, rows)
        passes = sa.find_passes(rows, 10.0, 0.25, sa.read_order(header), dump)[0]
        share = [sa.saturated_share(rows, p["rows"], dump["decim"]) for p in passes]
        self.assertEqual([s[0] + s[1] + s[3] for s in share], [0, 0, 0], "only LO 5825 clipped")
        self.assertGreater(share[0][2], 0.1, "R6's first pass: 100 ms clipped of a few hundred")
        self.assertLess(share[0][2], 0.6)
        self.assertEqual(share[1][2], 0.0, "R6's second pass did not clip")
        self.assertEqual(sa.sat_text(share[0], sa.read_groups(header)).split()[:3], ["saturated", "1+2", "0%"])
        old, _ = sa.read_scans(qdb_recording(self.EVENTS, 15), sat=True)
        self.assertIsNone(sa.saturated_share(old, passes[0]["rows"], 15), "an older row has no share")

    def test_a_count_reads_15_at_most(self):
        rows, header = sa.read_scans(qdb_recording([], 16, seconds=2, sat=[(0, 0.0, 9.0)]), sat=True)
        self.assertEqual({r[2][0] for r in rows}, {15})
        self.assertAlmostEqual(sa.saturated_share(rows, (0, len(rows) - 1), 16)[0], 15 / 16)

    def test_main_prints_the_share(self):
        for sat, expect in ((self.CLIP, True), (None, False)):
            with tempfile.TemporaryDirectory() as d:
                log = Path(d) / "run.log"
                log.write_text(qdb_recording(self.EVENTS, 15, sat=sat))
                out = io.StringIO()
                argv = sys.argv
                sys.argv = ["scan_analyze.py", str(log), "--margin", "10"]
                try:
                    with contextlib.redirect_stdout(out):
                        sa.main()
                finally:
                    sys.argv = argv
            lines = [line for line in out.getvalue().splitlines() if line.startswith("  t=")]
            self.assertEqual(len(lines), 3)
            if expect:
                self.assertRegex(lines[0], r"saturated 1\+2 0% 3\+4 0% 5\+6 [1-9]\d?% 7\+8 0%$")
                self.assertTrue(lines[1].endswith("saturated 1+2 0% 3+4 0% 5+6 0% 7+8 0%"))
            else:
                self.assertNotIn("saturated", out.getvalue())


class Analyze(unittest.TestCase):
    def run_on(self, text, margin=10.0):
        rows, header = sa.read_scans(text)
        self.assertIn("SCANDUMP end", header)
        return sa.find_passes(rows, margin, 0.25)[0]

    def test_passes_on_the_right_channel_and_time(self):
        passes = self.run_on(recording([(5, 3.0, 15, 0.15), (5, 7.0, 12, 0.15), (2, 10.0, 14, 0.15)]))
        self.assertEqual([p["ch"] for p in passes], [5, 5, 2])
        for p, at in zip(passes, (3.0, 7.0, 10.0)):
            self.assertLess(abs(p["t"] - at), 0.06, "smoothing delay stays small")

    def test_vtx_close_all_along_keeps_its_passes(self):
        # A VTX steady 12 dB over the others on R6, nine laps on it: each lap is a pass on R6.
        # (The rest quantile against a VTX parked most of the recording: AnalyzeQdb.)
        events = [(5, 2.0 + i, 15, 0.2) for i in range(9)]
        passes = self.run_on(recording(events, steady={5: 2.0}), margin=6.0)
        self.assertEqual([p["ch"] for p in passes], [5] * 9)

    def test_nothing_on_noise(self):
        self.assertEqual(self.run_on(recording([])), [])

    def test_bleed_goes_to_the_strongest(self):
        passes = self.run_on(recording([(5, 4.0, 18, 0.2), (4, 4.0, 2, 0.2), (6, 4.0, 3, 0.2)]))
        self.assertEqual(len(passes), 1)
        self.assertEqual(passes[0]["ch"], 5)
        self.assertEqual(len(passes[0]["bleed"]), 2)

    def test_the_highest_level_owns_a_shared_pass(self):
        # R7 rises more above its own median, but R6 is the stronger channel.
        steady = {5: 8.0, 6: -8.0}
        passes = self.run_on(recording([(5, 4.0, 22, 0.2), (6, 4.0, 10, 0.2)], steady=steady))
        self.assertEqual([p["ch"] for p in passes], [5])

    def test_sweep_order_sets_each_channels_time(self):
        self.assertEqual(sa.read_order("SCANDUMP end gain=40 order=1:5:2:6:3:7:4:8"), [0, 2, 4, 6, 1, 3, 5, 7])
        self.assertEqual(sa.read_order(""), list(range(8)))
        rows = [(k * PERIOD_US, [100] * 8) for k in range(10)]
        times, _ = sa.channel_times(rows, sa.read_order("order=1:5:2:6:3:7:4:8"))
        self.assertGreater(times[4][0], times[0][0])
        self.assertLess(times[4][0], times[1][0], "R5 is read before R2")

    def test_end_line_is_the_header(self):
        rows, header = sa.read_scans(recording([]).replace("SCANDUMP end", "SCANDUMP end gain=40 order=1:5:2:6:3:7:4:8"))
        self.assertIn("gain=40", header)
        self.assertGreater(len(rows), 10)

    def test_steady_interference_is_not_a_pass(self):
        steady = {0: 7.0, 3: 7.0}
        passes = self.run_on(recording([(5, 6.0, 15, 0.15)], steady=steady))
        self.assertEqual([p["ch"] for p in passes], [5])

    def test_bits_flickering_over_a_silent_channel_are_not_a_pass(self):
        rows = [(k * PERIOD_US, [0 if k % 40 else 40] * 8) for k in range(800)]
        self.assertEqual(sa.find_passes(rows, 10.0, 0.25)[0], [])

    def test_missing_samples(self):
        passes = self.run_on(recording([(5, 6.0, 15, 0.15)], drop=0.02))
        self.assertEqual([p["ch"] for p in passes], [5])

    def test_last_complete_dump_wins(self):
        first = recording([(1, 3.0, 15, 0.15)])
        second = recording([(6, 3.0, 15, 0.15)])
        self.assertEqual([p["ch"] for p in self.run_on(first + "\n" + second)], [6])

    def test_no_data_power(self):
        self.assertEqual(sa.to_db(0), sa.FLOOR_DB)
        self.assertAlmostEqual(sa.to_db(256), 0.0)
        self.assertTrue(math.isclose(sa.to_db(2560), 10.0))


if __name__ == "__main__":
    unittest.main()
