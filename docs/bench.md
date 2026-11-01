# Bench procedure

Hardware: the receiver, a VTX on a Raceband channel at fixed power, the video on
a screen. Propulsion removed. Console: `python tools/flash.py PORT
--monitor-only --log run.log`.

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
- In `T`, `samples_valid` must dominate `unknown` when video is received. **If
  `unknown` dominates with a good picture, report it to me**: another source
  would be needed, e.g. the dBm from `receiver_level()` updated at 1 Hz.
- Stop the trace (`K` `l`) before the quality measurements.

## 3. Detector (no audio)

| Test | Expected |
| --- | --- |
| Move the VTX toward the antenna then away, over about 2 s | one `PASS` line, with `delay_ms` < 1000 |
| Same pass, 5 times at least 3 s apart | 5 `PASS` lines |
| VTX held still near the antenna for 30 s | a single `PASS` (or none) |
| VTX off, or far, for 2 min | no `PASS` |
| VTX at medium distance (peak below `min_peak_db`) | no `PASS` |
| `c` (next channel) during a pass | no phantom `PASS`; `semitones` changes with the channel |
| `o` (menu) or `s` (scan) during a pass | no `PASS`; in `T`, `unknown` rises |

Tune with `K`: `t`/`T` and `m`/`M` from the measured `peak_cn_db`, then `d`/`D`
for responsiveness.

## 4. Audio (after rewiring and an audio build)

- `P` on each channel, R1 to R8 (`c` to advance): the pitch rises from R1 to R8.
  `semitones` shows −7, −5, −3, 0, +2, +5, +7, +9.
- `K` `v`/`V`: the volume changes. `p` mutes the sound (`audio=muted` in `PASS`),
  then `p` restores it.
- On a scope or the LED: SD goes high about 5 ms before the sound and returns low
  about 50 ms after. No click at the start or end.
- `P P P P` in a burst: at most 3 sounds (one playing and two queued); `dropped`
  rises in `T`.
- During a sound, `g` (OUTPUT, transport restart): the video returns and the next
  `P` sounds. An isolated `AUDIO error=write` and `failed` +1 are expected.

## 5. Receive quality, audio active or not

With a fixed setup (same VTX, channel, power, distance), without touching
anything during the measurement:

```sh
python3 tools/checkpoint_bench.py PORT --seconds 120 --every 2 --out bench_audio
```

The script records the `FIELD_DIAG` stream in three phases: A1 with no sound, B
with `P` every 2 s, A2 with no sound. It then compares them with
`tools/field_diag_compare.py`.

- A1 and A2 must agree with each other: that is the setup's drift.
- B must not be worse than A1 and A2 on `sync_evidence_ok_pct`,
  `coverage_pct_median`, `cn_est_median_db`, `chain_fault_s` and `dropouts`. A B
  gap outside the A1/A2 gap indicates an effect of the audio.
- Repeat at the edge of range (attenuator or distance), where any degradation
  would be most visible.
- Do the same once with the `K` `l` trace active: it loads the USB console and is
  not meant for flight.
