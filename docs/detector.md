# Detector: operation and parameters

Pass detection on the tuned channel (`components/pass_detector`, pure C).

- **Signal:** `receiver_obs_cn_x10()`, the tuned channel's carrier-to-noise
  ratio (C/N) in dB, relative to a noise reference. It is read every 50 ms by a
  priority-1 task, below every receive task (control and gain_obs at 3,
  gain_fast at 4).
- **No sample** while the boot splash window, a calibration or a pause owns
  the receiver, nor when the C/N is unknown (gain settling, no noise reference,
  clipping). After `stale_ms` with no sample the channel returns to IDLE. A
  channel change resets the detector.
- **Read-only:** no GPIO, DMA, channel change or gain write. The MODEM_DIAG,
  BitScrambler, PARLIO and AGC chain is not modified.
- **State machine** per channel: `IDLE → RISING → PEAK_PASSED → COOLDOWN` (see
  `pass_detector.h`). A hovering drone only sounds once: to re-arm, the
  refractory period must have elapsed **and** the level must have dropped back
  under `active_db − hysteresis_db`.
- **One line per pass:**
  `PASS channel=R3 freq=5732 peak_cn_db=31.4 peak_ms=… at_ms=… delay_ms=…`, and
  sent over the UART link as an `EVENT_PASS` (the C6 master plays the sound).

## Parameters (console `K`, saved in NVS "checkpoint")

| Parameter | Default | Keys |
| --- | --- | --- |
| `alpha` (exponential smoothing, weight of a new sample) | 0.30 | `a`/`A` ±0.05 |
| `active_db` (activity threshold, C/N) | 15 dB | `t`/`T` ±1 dB |
| `hysteresis_db` | 3 dB | `h`/`H` ±0.5 dB |
| `drop_db` (fall below the peak that marks a pass) | 3 dB | `d`/`D` ±0.5 dB |
| `min_peak_db` (below it, the drone is too far: no pass) | 25 dB | `m`/`M` ±1 dB |
| `refractory_ms` | 1500 ms | `r`/`R` ±250 ms |
| `stale_ms` | 1000 ms | (fixed) |

Other keys: `K` then `l` (live C/N trace, 20 lines/s), `0` (defaults), `k`
(show), `s` (hop bench: [bench.md](bench.md#4-hop-bench-can-one-c5-scan-raceband)),
`S` (scan prototype, `g`/`G` its gain: [bench.md](bench.md#5-scan-prototype-do-real-passes-stand-out)),
`z` (residue trace: same page), `i` (antenna onboard/IPEX, C5-Zero only: GPIO 26),
`q` (close). The `K` window stays open 5 s after each key. At the top
level, `T` prints the `CHECKPOINT` line.

**Setting `active_db`:** put it below the C/N of a drone passing the gate, but
above the C/N of the same drone elsewhere on the track. Otherwise the channel
never re-arms between laps.
