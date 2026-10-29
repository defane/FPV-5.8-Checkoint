#!/usr/bin/env python3
"""Checkpoint bench: does the coin sound disturb reception? A/B/A run.

With a VTX on a fixed bench setup (same channel, power, distance), records
the per-second FIELD_DIAG stream (console R) in three phases and compares
them with tools/field_diag_compare.py:

  A1  quiet:  no sound
  B   audio:  console P (coin at the tuned channel's pitch) every --every s
  A2  quiet:  no sound again (drift check: A1 and A2 should agree)

  python3 tools/checkpoint_bench.py PORT [--seconds 60] [--every 2] [--out DIR]

T (status, heap and the CHECKPOINT line) is captured before and after. Run
it with the audio build on the rewired amplifier; on a build without audio
phase B only exercises the console path. Logs go to DIR/a1.log, b.log,
a2.log and DIR/status.log.
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flash import open_quiet  # noqa: E402  (same quiet open: no chip reset)
import field_diag_compare  # noqa: E402


def capture(s, seconds, log, key_every=0.0, key="P"):
    end = time.monotonic() + seconds
    next_key = time.monotonic() + key_every if key_every else None
    with open(log, "w", encoding="utf-8") as f:
        while time.monotonic() < end:
            data = s.read(4096)
            if data:
                f.write(data.decode(errors="replace"))
            if next_key is not None and time.monotonic() >= next_key:
                next_key += key_every
                s.write(key.encode())
                f.write(f"\n# key {key}\n")


def send_and_read(s, key, wait=1.5):
    s.write(key.encode())
    end, out = time.monotonic() + wait, ""
    while time.monotonic() < end:
        out += s.read(4096).decode(errors="replace")
    return out


def set_stream(s, on):
    """R toggles the FIELD_DIAG stream; it answers 'FIELD_DIAG stream=N'."""
    for _ in range(2):
        out = send_and_read(s, "R", 1.0)
        if f"FIELD_DIAG stream={1 if on else 0}" in out:
            return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--seconds", type=float, default=60.0, help="per phase (default 60)")
    ap.add_argument("--every", type=float, default=2.0, help="seconds between sounds in phase B (default 2)")
    ap.add_argument("--out", type=Path, default=Path("checkpoint_bench"))
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    s = open_quiet(a.port)
    try:
        status = send_and_read(s, "T", 2.0)
        if not set_stream(s, True):
            sys.exit("no 'FIELD_DIAG stream=1' answer: is the receiver console on this port?")
        print(f"A1 quiet {a.seconds:.0f} s ...")
        capture(s, a.seconds, a.out / "a1.log")
        print(f"B  coin every {a.every:g} s for {a.seconds:.0f} s ...")
        capture(s, a.seconds, a.out / "b.log", key_every=a.every)
        print(f"A2 quiet {a.seconds:.0f} s ...")
        capture(s, a.seconds, a.out / "a2.log")
        set_stream(s, False)
        status += send_and_read(s, "T", 2.0)
    finally:
        s.close()
    (a.out / "status.log").write_text(status, encoding="utf-8")
    for line in status.splitlines():
        if line.startswith(("CHECKPOINT", "C5VRX5 version")):
            print(line)
    print()
    return field_diag_compare.main(["field_diag_compare", *(str(a.out / n) for n in ("a1.log", "b.log", "a2.log"))])


if __name__ == "__main__":
    sys.exit(main())
