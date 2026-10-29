#!/usr/bin/env python3
"""Checkpoint coin sound: assets/coin_source.mp3 -> raw PCM for the firmware.

Output: components/audio_player/sounds/coin.pcm, signed 16-bit little-endian,
mono, 22050 Hz, no header (embedded with EMBED_FILES). Steps, all in ffmpeg:

  1. decode, downmix to mono, resample to 22050 Hz;
  2. cut the leading and trailing silence (below -50 dBFS);
  3. 2 ms fade-in and 10 ms fade-out (no click at either end);
  4. peak-normalise to -3 dBFS (measured by volumedetect on steps 1-3,
     then one fixed gain: a second pass, so the result is reproducible).

  python3 tools/coin_sound.py              # convert (needs ffmpeg on PATH)
  python3 tools/coin_sound.py --check      # verify the committed PCM only

The source is not versioned (assets/README.md: author and licence). The same
source and ffmpeg give the same bytes; the SHA-256 is printed for the commit.
"""
import argparse
import hashlib
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "assets" / "coin_source.mp3"
OUTPUT = ROOT / "components" / "audio_player" / "sounds" / "coin.pcm"

RATE_HZ = 22050
SILENCE_DB = -50
FADE_IN_S = 0.002
FADE_OUT_S = 0.010
PEAK_DBFS = -3.0
PEAK_TOLERANCE_DB = 0.5     # --check: s16 rounding and resampler overshoot
MAX_SECONDS = 3.0           # a coin, not a song: the flash image stays small


def shaping_filter():
    # silenceremove only trims a start: reverse to trim the end the same way.
    # afade=t=out needs the duration, so the fade-out is a fade-in on the
    # reversed stream.
    trim = f"silenceremove=start_periods=1:start_threshold={SILENCE_DB}dB:start_silence=0"
    return ",".join([
        f"aresample={RATE_HZ}", trim, "areverse", trim,
        f"afade=t=in:d={FADE_OUT_S}", "areverse", f"afade=t=in:d={FADE_IN_S}",
    ])


def ffmpeg(args):
    cmd = ["ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "info", "-y", *args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"ffmpeg failed ({r.returncode}):\n{r.stderr[-2000:]}")
    return r.stderr


def measure_peak_db(source):
    log = ffmpeg(["-i", str(source), "-ac", "1", "-af", shaping_filter() + ",volumedetect", "-f", "null", "-"])
    m = re.search(r"max_volume:\s*(-?[\d.]+) dB", log)
    if not m:
        sys.exit("volumedetect printed no max_volume (empty sound after trimming?)")
    return float(m.group(1))


def check(path):
    data = path.read_bytes()
    if not data or len(data) % 2:
        return f"{path}: empty or odd length ({len(data)} bytes)"
    n = len(data) // 2
    samples = struct.unpack(f"<{n}h", data)
    peak = max(abs(s) for s in samples)
    peak_db = 20 * math.log10(peak / 32768) if peak else -math.inf
    seconds = n / RATE_HZ
    print(f"{path}: {n} samples, {seconds:.3f} s at {RATE_HZ} Hz, peak {peak_db:.2f} dBFS, "
          f"first {samples[0]} last {samples[-1]}, sha256 {hashlib.sha256(data).hexdigest()}")
    if seconds > MAX_SECONDS:
        return f"longer than {MAX_SECONDS} s"
    if abs(peak_db - PEAK_DBFS) > PEAK_TOLERANCE_DB:
        return f"peak {peak_db:.2f} dBFS, expected {PEAK_DBFS} +- {PEAK_TOLERANCE_DB}"
    edge = 32768 * 10 ** ((PEAK_DBFS - 20) / 20)   # the fades start and end near silence
    if abs(samples[0]) > edge or abs(samples[-1]) > edge:
        return "first or last sample not near zero (fades missing?)"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="only verify the existing PCM")
    ap.add_argument("--source", type=Path, default=SOURCE)
    ap.add_argument("--output", type=Path, default=OUTPUT)
    a = ap.parse_args()
    if not a.check:
        if not a.source.exists():
            sys.exit(f"{a.source} not found: download it first (assets/README.md)")
        gain_db = PEAK_DBFS - measure_peak_db(a.source)
        a.output.parent.mkdir(parents=True, exist_ok=True)
        ffmpeg(["-i", str(a.source), "-ac", "1", "-af", f"{shaping_filter()},volume={gain_db:.2f}dB",
                "-map_metadata", "-1", "-c:a", "pcm_s16le", "-f", "s16le", str(a.output)])
        print(f"gain {gain_db:+.2f} dB")
    if not a.output.exists():
        sys.exit(f"{a.output} not found: run without --check first")
    err = check(a.output)
    if err:
        sys.exit(f"FAIL {err}")
    print("ok")


if __name__ == "__main__":
    main()
