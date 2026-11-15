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
  `PASS channel=R3 freq=5732 peak_db=15.4 peak_ms=… at_ms=… delay_ms=… comp_ms=63 est_peak_ms=… tau_ms=61 held=0`,
  and sent over the UART link as an `EVENT_PASS` (the C6 master plays the sound), timed
  at the smoothed peak `peak_ms`. The smoothing delays that peak by up to τ: `comp_ms`
  (τ plus one reading interval) and `est_peak_ms` (`peak_ms − comp_ms`) estimate the true
  peak, on the console only. The wire keeps the smoothed peak: every gate lags alike, so
  laps are unaffected ([fleet.md](fleet.md), "Limit"). `held` counts the channel's readings
  the pair guard held since its previous pass ([below](#pair-ghost-guard)), 0 on most passes.
- **Side by side:** two channels peaking together are two passes. No pass is dropped as
  bleed from another channel. The two channels of an LO share a capture, and R1's image
  falls next to R2 (the same for every pair): the I/Q correction took the image rejection
  to 55–58 dB in widscan58's offline measurements; bench B3 (`K` `x`) measures it on this
  board. A drone that clips the ADC is another matter: the [pair ghost guard](#pair-ghost-guard)
  holds its partner's readings meanwhile.

## Pair ghost guard

**What it fixes.** A capture carries the two channels of its LO. When a drone close to
the gate drives the ADC into its 10-bit rails, the rails' own products land in the other
channel's window, and that channel reads a level the strong drone made. On the C5-Zero
at gain 40 ([board] 2026-11-15, a 25 mW VTX on R6, which clips within about 30 cm of the
onboard antenna):

- R6 under −3 dBFS: R5, its partner, stays at its rest;
- R6 at −2 to −1 dBFS: R5 rises 4–13 dB (median);
- R6 at the ceiling: R5 rises 27–29 dB (median, 31.5 max), and the channels of the other
  LOs about 1 dB (4–7 dB max);
- live, many ghost `PASS` lines on R5 (`peak_db` 10–25), while R6's own passes read
  `peak_db` 53.4–53.7, the clipped plateau.

The I/Q correction cannot remove it: a perfect I/Q path gives the same products ([host]
model, the repository's DSP on synthetic 10-bit captures). They sit a median 23–29 dB
under the strong channel, a few % of captures above it ([host]).

**The rule** (`sd_feed_hop()` in `components/pass_detector/scan_detector.c`;
`cp_feed_hop()` in `cp_scan.c` feeds every hop through it):

- It looks only at a capture of two channels that carries `SDR_RD_SATURATED` (one flag per
  capture, on both readings). Inside a hover, a run of saturated captures that has lasted
  `SD_HOLD_HOVER_US` (150 ms; a pass at 20 m/s clips about 80 ms at 0.3 m for 600 mW,
  [host]), it also treats the clean captures up to `SD_HOLD_TAIL_US` (30 ms) after the
  last saturated one as saturated: a drone parked by the gate fades in and out of the
  rails.
- On such a capture, the reading of a channel whose smoothed level stands more than
  `SD_HOLD_DOM_DB` (15 dB) under its partner's is **held**: not fed, its median, average,
  rest and state frozen. The comparison uses the smoothed levels, never the capture's own
  readings: those are the strong drone's products. The strong channel is never held.
- When a held channel is fed again, its median restarts on fresh readings. If it stood
  `min_peak_db` up when the hold began (a real pass under way), its average catches up
  to the fresh median less `drop_db`, upwards only, so a catch-up never rings by itself.
  After a hold over `SD_HOLD_LONG_US` (0.5 s) it resumes in `COOLDOWN`: what follows is the
  tail of what it could not see.
- A capture without the flag (outside a hover's tail) goes through exactly as before: the
  same passes to the bit ([host] `test_scan_detector.c` check 22).
- A held reading counts as none: a channel held for 1 s reads *unknown* on the bridges
  ([fleet.md](fleet.md)), with no fault and no badge.

`CONFIG_CHECKPOINT_PAIR_GUARD` (default y) turns it on; with n every reading is fed as
it comes, the ghosts included. Its Kconfig help lists the limits below. The constants are
the host evaluation's: retune them from bench B11's data, not from the host model.

**What the host model shows** ([host], 20 runs a point; `test/test_pair_ghost.c` runs
the bench case on the repository's DSP, `test/test_scan_detector.c` checks 22–26 a
reading model):

- a lone 600 mW drone passing 0.1–0.5 m away at 20 m/s: 80 ghost passes in 80 runs
  without the guard, 0 with it; hand-held 25 mW at 5–30 cm: 0 in 140 runs; with the lower
  channel of a pair strong (R5 clipping, the ghost on R6; 280 runs, fast and hand-held):
  163 ghost passes without the guard, 0 with it;
- a real partner up to 40 dB weaker, crossing 0, 50 or 200 ms after the clipping drone:
  420 kept of 420 (without the guard the ghost took the pass at +200 ms: 1 kept of 20 at
  30 dB); 45 dB weaker at the same instant it cannot be measured at all (0 of 20);
- the timing cost: a partner peaking within ±50 ms of a clipping drone is timed from the
  readings around the clip window, mean −24 to +23 ms, 73 ms at worst.

**Its limits.**

- **The partner is blind while the strong drone clips**: about 80 ms for 600 mW at
  0.3 m and 20 m/s. The window grows as 1/speed (about 310 ms at 5 m/s, where a partner
  20–30 dB weaker crossing at the same instant rang on time in 5 runs of 20 and
  150–170 ms late in the others; check 26 keeps it as a known loss) and with the link gain (the IPEX connector with a better antenna, or a
  directional one; about 870 ms at 5 m/s with about 8 dB more). A 25 mW drone at a
  distance d clips like a 600 mW one at 4.9 d.
- **It keys on the flag only.** A compression before the ADC rails (no rail hit, no flag)
  is not covered: with that, the host model rang as many ghosts as without the guard.
  Bench B11 (B-g2) checks whether the C5 does it.
- **Other LOs**: the +4–7 dB rises seen on the other LOs at the ceiling are not the pair's
  products, and the guard does not touch them (B11, B-g6).
- **A clean image**: if the unclipped image rejection were only 45 dB, one hand-held ghost
  of 10 dB would remain ([host]). Bench B3 measures it (≥ ~49 dB, floor-limited, [board]
  2026-11-15).
- **IPEX**: the `detector-ipex` build keeps the default until bench B9 measures the IPEX
  link; decide its default then.

**On the board.**

- `SDR ready … pair_guard=1` at boot: the build has the guard. An existing build dir does
  not see a new Kconfig option ([host] 2026-11-15: ESP-IDF 6.0.2 kept the old sdkconfig and
  built without it): after pulling it, `tools/fw.sh clean`, then build.
- `K` `l`: `sat=` counts each LO's saturated captures and `held=` each channel's held
  readings, both since the previous line ([bench.md](bench.md#2-signal)).
- `PASS … held=`: the channel's held readings since its previous pass.
- `K` `S`: each row's saturated captures per LO, and `tools/scan_analyze.py`'s saturated
  share per pass ([bench.md](bench.md#5-scan-recording-do-real-passes-stand-out)).
- `K` `s`: `detector_us` includes the guard (the hop's time over its readings).
- Bench B11 ([bench.md](bench.md#7-first-bench-b0-to-b11-and-results)) is the board check.

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

Other keys: `K` then `l` (live rise of R1-R8 and each LO's saturated captures, `CP_TRACE`
lines, 5 a second), `0`
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
