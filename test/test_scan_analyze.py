#!/usr/bin/env python3
"""tools/scan_analyze.py on synthetic recordings: passes, bleed, steady interference."""
import importlib.util
import math
import random
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

    def test_vtx_close_most_of_the_time_keeps_its_passes(self):
        # Held close for most of the recording: the median is no longer the rest.
        events = [(5, 2.0 + i, 15, 0.2) for i in range(9)]
        passes = self.run_on(recording(events, steady={5: 2.0}), margin=6.0)
        self.assertTrue(passes and all(p["ch"] == 5 for p in passes))

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
