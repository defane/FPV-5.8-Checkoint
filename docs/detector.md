# Detector: operation and parameters

The C5 reads R1 to R8 all the time and detects a pass on any of them: nobody picks a
channel (`components/checkpoint/cp_scan.c`, `checkpoint.c`; pure C:
`components/pass_detector/scan_detector.c`, `pass_detector.c`).

- **Signal:** the sdr_rx engine (`components/sdr_rx`) reads two channels per LO: 5677 MHz
  carries R1 and R2, 5751 MHz R3 and R4, 5825 MHz R5 and R6, 5899 MHz R7 and R8 (each pair
  19 MHz below and 18 MHz above its LO). A hop takes a 256-sample snapshot of the PHY's
  I/Q dump at 80 MS/s (3.2 µs), tunes the next LO (PLL only), then computes a 4 × 64-point
  integer PSD of the snapshot while that LO settles; the I/Q image is corrected from the
  I/Q moments of the LO's recent captures. A channel's level is its band power within
  ±5 MHz, in 0.25 dB codes (dBFS × 4). A rotation is the 4 hops, in ascending LO order;
  widscan58 measured about 510 rotations a second, so about 510 readings a second per
  channel (bench B1 measures this board's). Each reading carries the time of its capture.
- **Gain:** forced, no AGC: index 40 by default (widscan58's preset for the C5-Zero's
  onboard antenna). `K` then `g`/`G` moves it by 2, between 10 and the PHY's maximum,
  saved; the rests are learnt again after a change.
- **Smoothing:** per channel, a median of the last 3 readings (a lone burst, a Wi-Fi frame
  say, vanishes), then an exponential average weighted by each reading's interval.
  `alpha` is the weight per 17.5 ms reference sweep, the 57 Hz the defaults were tuned
  at, so the time constant τ = −17.5 ms / ln(1 − alpha) is the same at any rate: 61 ms
  at 0.25.
- **Rest:** each channel has its own resting level in dB (the noise differs across the
  band). It follows the channel's low smoothed values (down at 1.14 dB/s, up at
  0.114 dB/s), ten times faster for the first 3 s, stops climbing while the channel is
  active, and may not climb more than 6 dB above the other channels' median. So a VTX
  left on keeps showing, and a drone waiting by the gate does not become the channel's
  rest. A clipped capture never moves a rest: compression reads the partner channel low.
- **Level:** the rise of the smoothed level above the rest, in dB, is what goes to the
  state machine and what the web pages show. There is no signal floor: the 10-bit
  samples resolve the noise.
- **Gaps:** no reading while a calibration or a console bench runs. A channel with no
  reading for over 100 ms restarts its median and holds its next two readings until the
  median is full again, so a lone burst right after the gap stays a lone burst; its average
  then goes on from where it was, so a VTX parked across a PHY-tracking recalibration
  (~0.2 s) rings nothing. After a gap longer than `stale_ms` the state machine has
  forgotten the channel, and the average starts again from that median. A late reading
  (over twice the usual interval) counts in the average as one on time. After a requested
  calibration (`F`, `B`, `K` `i`, a link command), a gain change or a bench the rests are
  learnt again: nothing rings for 3 s, as at boot.
- **State machine** per channel: `IDLE → RISING → PEAK_PASSED → COOLDOWN` (see
  `pass_detector.h`). A hovering drone only rings once: to re-arm, the refractory period
  must have elapsed **and** the level must have dropped back under
  `active_db − hysteresis_db`.
- **One line per pass:**
  `PASS channel=R3 freq=5732 peak_db=15.4 peak_ms=… at_ms=… delay_ms=… comp_ms=63 est_peak_ms=… tau_ms=61`,
  and sent over the UART link as an `EVENT_PASS` (the C6 master plays the sound), timed
  at the smoothed peak `peak_ms`. The smoothing delays that peak by up to τ: `comp_ms`
  (τ plus one reading interval) and `est_peak_ms` (`peak_ms − comp_ms`) estimate the true
  peak, on the console only. The wire keeps the smoothed peak: every gate lags alike, so
  laps are unaffected ([fleet.md](fleet.md), "Limit").
- **Side by side:** two channels peaking together are two passes. No pass is dropped as
  bleed from another channel. The two channels of an LO share a capture, and R1's image
  falls next to R2 (the same for every pair): the I/Q correction took the image rejection
  to 55–58 dB in widscan58's offline measurements; bench B3 (`K` `x`) measures it on this
  board.

## Parameters (console `K`, saved in NVS namespace "checkpoint", key "prefs")

| Parameter | Default | Keys |
| --- | --- | --- |
| `alpha` (smoothing weight per 17.5 ms reference, τ 61 ms at 0.25) | 0.25 | `a`/`A` ±0.05 |
| `active_db` (activity threshold, rise above the rest) | 8 dB | `t`/`T` ±1 dB |
| `hysteresis_db` | 3 dB | `h`/`H` ±0.5 dB |
| `drop_db` (fall below the peak that marks a pass) | 3 dB | `d`/`D` ±0.5 dB |
| `min_peak_db` (below it, the drone is too far: no pass) | 10 dB | `m`/`M` ±1 dB |
| `refractory_ms` | 1500 ms | `r`/`R` ±250 ms |
| `stale_ms` | 1000 ms | (fixed) |
| RX gain (forced index) | 40 | `g`/`G` ±2 (relearns the rests) |

The record is version 3 (`PREFS_VERSION`): the meter changed (band power, 10-bit,
clipping instead of folding), so the first boot of sdr_rx ignores a C5VRX-era record
and starts on the defaults. The bridges send the same fields (`CONFIG_SET`): `alpha`
keeps its meaning there.

The defaults are still the C5VRX meter's, tuned on hand passes at 57 Hz (4 out of 4 at
+13 to +16 dB at gain 24, weak bumps reaching +8 dB). They are kept until bench B6
records real passes on this meter, where a 25 mW drone at 0.9 m rises 22–30 dB
(widscan58), so a far 200 mW drone may still pass `min_peak_db`. Retune them then,
together with the C3's and the C6's copies (`firmware/node/main/c3_store.c`,
`firmware/master/main/c6_hub.c`), in one commit.

Other keys: `K` then `l` (live rise of R1-R8, `CP_TRACE` lines, 5 a second), `0`
(defaults), `k` (show), `i` (antenna onboard/IPEX, C5-Zero only: GPIO 26, not saved,
then every LO recalibrated on it), the engine benches `s`, `z`, `x`, `n`, `p` and `S`
([bench.md](bench.md#4-engine-bench)), `q` (close). The `K` window stays open 5 s after
each key. At the top level: `T` prints the `FIRMWARE`, `STATUS`, `SDR` and `CHECKPOINT`
lines, `F` recalibrates every LO, `B` recalibrates and verifies the image rejection (VTX
off for both, the gate is blind for about 3 s), `` ` `` reboots into the USB download
mode. The top-level keys are in `components/checkpoint/cp_console.c`, the `K` keys in
`checkpoint.c`.

**Setting `active_db`:** put it below the rise of a drone passing the gate, but above the
rise of the same drone elsewhere on the track. Otherwise the channel never re-arms between
laps.
