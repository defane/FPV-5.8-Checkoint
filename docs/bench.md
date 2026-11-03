# Bench procedure

Hardware: the detector (C5), a VTX on a Raceband channel at fixed power.
Propulsion removed. The detector has no picture and no sound: everything shows
on the console (`python tools/flash.py PORT --monitor-only --log run.log`); the
coin sound is the C6 master's ([fleet.md](fleet.md)).

## 1. Build, flash, startup

```sh
( cd firmware/detector && idf.py build )            # → build/detector-c5.bin (0 warnings expected)
python tools/flash.py PORT --check
```

- Startup ends with `C5VRX5 … ready`. There is no `CHECKPOINT disabled` line.
- With `T`: a `CHECKPOINT source=cn …` line. Note `heap_internal`, `heap_min` and
  `heap_largest`, and compare with the upstream firmware (same board). `heap_min`
  must not drop: the task starts after boot.
- Startup prints `CHECKPOINT video_output=off`. There is no picture: that is
  normal.

## 2. Signal source

- Send `K` then `l` (trace). With the VTX on and close, `CP_TRACE … cn_db=` shows
  stable values. With the VTX off, you read `cn_db=unknown` or a low value.
- In `T`, `samples_valid` must dominate `unknown` with the VTX on and close.
  **If `unknown` dominates with a strong VTX, report it to me**: another source
  would be needed, e.g. the dBm from `receiver_level()` updated at 1 Hz.
- Stop the trace (`K` `l`) before the quality measurements.

## 3. Detector

| Test | Expected |
| --- | --- |
| Move the VTX toward the antenna then away, over about 2 s | one `PASS` line, with `delay_ms` < 1000 |
| Same pass, 5 times at least 3 s apart | 5 `PASS` lines |
| VTX held still near the antenna for 30 s | a single `PASS` (or none) |
| VTX off, or far, for 2 min | no `PASS` |
| VTX at medium distance (peak below `min_peak_db`) | no `PASS` |
| `c` (next channel) during a pass | no phantom `PASS`; `channel=` follows the new channel |

Tune with `K`: `t`/`T` and `m`/`M` from the measured `peak_cn_db`, then `d`/`D`
for responsiveness.
