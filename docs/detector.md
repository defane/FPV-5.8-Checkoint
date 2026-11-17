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
  must have elapsed **and** either the level must have dropped back under
  `active_db − hysteresis_db`, or its 1 s average must stand the re-arm drop D (16 dB by
  default) under the highest level since the pass (the [relative re-arm](#relative-re-arm),
  for a drone that stays up between laps). After a relative re-arm, the next pass must also
  rise D over the lowest 1 s average since.
- **One line per pass:**
  `PASS channel=R3 freq=5732 peak_db=15.4 peak_ms=… at_ms=… delay_ms=… comp_ms=63 est_peak_ms=… tau_ms=61 held=0 rearm=abs`,
  and sent over the UART link as an `EVENT_PASS` (the C6 master plays the sound), timed
  at the smoothed peak `peak_ms`. The smoothing delays that peak by up to τ: `comp_ms`
  (τ plus one reading interval) and `est_peak_ms` (`peak_ms − comp_ms`) estimate the true
  peak, on the console only. The wire keeps the smoothed peak: every gate lags alike, so
  laps are unaffected ([fleet.md](fleet.md), "Limit"). `held` counts the channel's readings
  the pair guard held since its previous pass ([below](#pair-ghost-guard)), 0 on most passes.
  `rearm` tells how the channel was armed for this pass: `abs` (it fell under the release)
  or `rel` (the [relative re-arm](#relative-re-arm)).
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
  rest and state frozen. The comparison uses the smoothed levels, not one capture's
  readings: the products top their source on a few % of captures ([host]).
- **A jump into the rails.** A VTX switched on or armed beside the antenna clips at once,
  while its average is still low. The smoothed levels alone then hold the wrong channel
  when the partner stands up: the source is held, frozen low for the whole run, and the
  partner is fed its products (it rang a phantom 150 ms after the onset, [host]). When the
  median of a channel's last 3 readings stands more than 15 dB over its partner's and
  more than `SD_HOLD_JUMP_DB` (20 dB) over its own average, it is the run's source: its
  partner is held until the run ends. The products top their source by 12.8 dB at most
  on a capture ([host] DSP), so they never pass for it: that would take two of the
  source's 3 readings over their median. The median, not the lowest of the 3: a clipping
  source's FM dips 6–22 dB on a quarter of its readings, which kept it unnamed for up to
  18 captures while a partner at the floor was fed its products (a phantom on 1–3 % of
  the onsets within 10 cm, [host] DSP readings; none with the median). A drone flown in
  leads its average by 9 dB at most at 20 m/s, 16 dB at 60 m/s ([host] reading model),
  so it never takes this path as it flies in: the host evaluation below (its scenarios
  and its seven model variants) gives the same results to the byte. With the partner up, the source is
  still held on its first clipped capture ([host] check 29; with the FM spread 1.5
  captures on average, 5 at most).
- **The jump rule's limit.** It needs the partner more than 15 dB under the source's
  rails reading, so about 40 dB over the floor at most (the rails stand 54–58 dB over
  it). Over that, a quad left powered about 1 m from the antenna while another is armed
  beside it, the source is held for its whole run as by the smoothed levels alone, and
  its own onset rings nothing; every lap of the partner is still kept ([host], the rails
  55 dB over the floor: the source rang in 16 of 20 runs with the partner 44 dB up, in
  none from 46 dB).
- **A source named partway through a run.** A held channel's average is frozen. A drone
  that crosses close while another one clips leads it, so when the clipping drone's FM
  dips on 2 of its last 3 readings, the crossing drone is named the source partway through
  that run, and the clipping drone is held to its end. The crossing drone rings its pass
  (it was blind otherwise). The clipping drone loses its own pass when more than 0.5 s of
  its run is left: a slow pass, which clips 0.9–1.3 s at 2–3 m/s and 0.3 m for 600 mW
  ([host]). Race harness on the DSP readings ([host]; R6 at 600 mW passing 0.3 m away at
  2–5 m/s, R5 at 25 mW crossing 0.1–0.5 m away within 0.6 s of it; 47520 laps each): R6
  missed 7260 laps, against 6979 when the lowest of the 3 readings named the source, which
  seldom named one partway through a run; R5 missed 21004 against 21752. The name also
  helps: with R6 crashed on the gate, clipping for 20 s, a drone lapping 0.2 m from the
  antenna, once named, stays fed and rings its later laps (R5 missed 6846 of 16200, 7481
  with the lowest of the 3). Losing the name once the source had gone gave R6 its laps
  back (6812) but cost the crashed case (7895), so the name stays to the end of the run
  ([host] check 35).
- When a held channel is fed again, its median restarts on fresh readings. If it stood
  `min_peak_db` up when the hold began (a real pass under way), its average catches up to
  the fresh median less `drop_db`, upwards only, so a catch-up never rings by itself.
  After a hold over `SD_HOLD_LONG_US` (0.5 s) what it did while blind is unknown, so once
  its fresh median is full its average starts again from that median, whatever the hold's
  length. One median of a VTX's channel can sit 9–14 dB under its level, or 5 dB over it
  (the FM swings the band power, [host]). So the state does not start from it: it follows
  the fresh average, ringing nothing, for `SD_HOLD_SETTLE_TAUS` (5) time constants, 0.3 s
  at the default `alpha`, then starts again from it (`pd_resume()`). Under the release the
  channel is armed. Over it, it is relatively armed with that level as its trough (with
  the re-arm off, in `COOLDOWN` until it falls under the release). A shorter hold during
  the settle pauses it: the settle counts only the time the channel is fed. A hold over
  0.5 s starts it again. When every hold that began meanwhile counted as long, each one
  restarted the settle. A drone hovering at the clip edge then kept its partner ringing
  nothing for the whole hover ([host] check 34, one clipped capture every 200 ms for
  5.3 s: the partner's lap rang in 0 of 40 runs, now in 40; race harness on the DSP readings,
  R6 hovering 20 s 0.17 m from the antenna while R5 laps at 1 m: R5 kept 20 of 348 laps
  during the hover, now 58, and 67 without the settle). The falling tail of a pass it
  missed does not rise the re-arm's 16 dB over that level, so it rings nothing; its next
  pass does ([host] check 28). Started from the median itself, a still VTX 12–35 dB up
  rang a phantom after 3–11 % of 2 s holds ([host] readings drawn from the DSP, 1000 holds
  per level; check 30's model: 5 of 200); from the settled average, none of them. A pass
  that began to rise while the channel was blind, or in the 0.3 s of readings after, must
  still rise 16 dB over the level it comes back at, or it is lost with the blind window.
- A capture without the flag (outside a hover's tail) goes through exactly as before: the
  same passes to the bit ([host] `test_scan_detector.c` check 22).
- A held reading counts as none: a channel held for 1 s reads *unknown* on the bridges
  ([fleet.md](fleet.md)), with no fault and no badge.

`CONFIG_CHECKPOINT_PAIR_GUARD` (default y) turns it on; with n every reading is fed as
it comes, the ghosts included. Its Kconfig help lists the limits below. The constants are
the host evaluation's: retune them from bench B11's data, not from the host model.

**What the host model shows** ([host], 20 runs a point; `test/test_pair_ghost.c` runs
the bench case on the repository's DSP, `test/test_scan_detector.c` checks 22–26 and 29 a
reading model):

- a lone 600 mW drone passing 0.1–0.5 m away at 20 m/s: 80 ghost passes in 80 runs
  without the guard, 0 with it; hand-held 25 mW at 5–30 cm: 0 in 140 runs; with the lower
  channel of a pair strong (R5 clipping, the ghost on R6; 280 runs, fast and hand-held):
  163 ghost passes without the guard, 0 with it;
- a real partner up to 40 dB weaker, crossing 0, 50 or 200 ms after the clipping drone:
  420 kept of 420 (without the guard the ghost took the pass at +200 ms: 1 kept of 20 at
  30 dB); 45 dB weaker at the same instant it cannot be measured at all (0 of 20);
- the timing cost: a partner peaking within ±50 ms of a clipping drone is timed from the
  readings around the clip window, mean −24 to +23 ms, 73 ms at worst;
- a VTX switched on 0.3 m away for 0.1–0.3 s while a pilot on its partner laps 18 or
  25 dB up, or at the floor (check 29, 120 runs, both channel orders): every lap kept, no
  phantom, and the source rings its own pass. By the smoothed levels alone: a phantom in
  44 runs (all 40 with the pilot at the floor), and no pass of the source in 80. The same
  with the DSP's FM spread in the reading model (120 more runs); naming the source from
  the lowest of its 3 readings, there, rang a phantom in 4.

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

## Relative re-arm

**What it fixes.** A channel re-armed only once its level fell back under the release,
`active_db − hysteresis_db` (5 dB over its rest). A drone that stays well up between laps
never gets there, so every lap after the first was lost:

- a 25 mW VTX held 3 m away indoors stays about +30 dB, and `scan_analyze.py` found one
  22.5 s pass ([board] 2026-11-15, `K` `S`);
- a 600 mW drone 30–80 m out reads 15–23 dB, and an indoor 25 mW track with its far side
  5–15 m away reads 15–25 dB ([host] model).

The rests cannot help. A rest climbs only in plain IDLE, never in COOLDOWN, and never more
than 6 dB over the other channels' median.

**The rule** (`pass_detector.c`; the drop D, 16 dB by default, 0 = off, is a detector
parameter: [below](#setting-d)):

- **The re-arm.** Once the refractory period is over, COOLDOWN also goes to IDLE when the
  level's 1 s average (`PD_REARM_TAU_MS`) stands D under the highest level since the pass.
  That highest level follows the level up in COOLDOWN, so an early ring does not leave it
  low. A fade shorter than the average does not re-arm: two 20 dB fades of 150 and 300 ms
  inside a held pass ring once ([host] `test_pass_detector.c`), and at the reading rate a
  25 dB one of 600 ms too ([host] `test_scan_detector.c` check 27).
- **The prominence.** After a relative re-arm the level is still far above `active_db`, so
  the next 3 dB wobble (`drop_db`) would ring a phantom at a high level. The next pass must
  therefore also rise D over the trough, the lowest 1 s average since the re-arm. RISING
  needs the level over both `active_db` and `trough + D`, so the rest of the fall and its
  wobble ring nothing.
- **Back to absolute.** Falling under the release ends the relative mode: the channel is
  then armed exactly as before. With D = 0, or when the level always falls under the
  release between laps, the passes are the same to the bit ([host] tests and the
  simulation below).
- **The rest.** A relatively re-armed channel is not quiet: its rest does not climb.
- **The pair guard.** A channel back from a hold over `SD_HOLD_LONG_US` is relatively
  armed at the level it comes back at, once its fresh average has settled (armed as usual
  under the release), for any hold length (`pd_resume()`, [above](#pair-ghost-guard)).
  Forced into COOLDOWN with the level from before the hold as its reference, a channel that
  stays up between laps lost its next lap after any hold over 0.5 s: in check 28, the 17 s
  lap in 20 runs of 20, and in a run whose hold covered a lap, the lap after it too ([host]).

**Why 16 dB** ([host] simulation of races, 2026-11-15: the repository's detector, fed
readings drawn from its DSP on synthetic captures; its key cases are host tests:
`test_pass_detector.c`, `test_scan_detector.c` checks 27–28). The simulated races:

- outdoor laps at 25 and 600 mW, the gate pass 1–3 m away, the far side 30–80 m, 15–40 s
  laps; indoor 25 mW tracks, 8–15 s laps, the far side 5–15 m; 600 mW indoors; the IPEX
  antenna (+8 dB);
- deep fades inside each pass; tracks that come back near the gate; 10 s hovers and
  crashes by the gate; 30 s on a start pad 2 m away; two drones on one LO pair with the
  guard on;
- 40 runs a scenario, 7616 laps in all.

Fading: mild (1.5 dB slow plus 1 dB per capture), or rough (2.5–4 dB slow plus deep
notches). A parked drone wobbles like a VTX held at 3 m on the bench: sd 5.3 dB, 0.45 s
([board] recordings).

- The current firmware (D = 0) misses 98 % of the laps.
- Total lap errors (missed plus extra) are lowest for D 14–15 with mild fading and 16–18
  with rough fading.
- D 16 is also the smallest D that rings no extra pass on the bench's recording of a VTX
  held at 3 m. Replayed through the detector, that recording rings one extra pass at D 15
  and two at D 12 ([board] recording).

What D 16 costs ([host], laps missed % / extra %, mild and rough fading):

| case | mild | rough |
| --- | --- | --- |
| outdoor 600 mW | 3 / 4 | 7 / 10 |
| indoor 25 mW, the antenna at the gate (0.3–1 m) | 0 / 0 | 6 / 9 |
| indoor 25 mW, the antenna 1–3 m from the line | 32 / 0 | 20 / 10 |
| 600 mW indoors, IPEX included | 48 / 0 | 28 / 9 |
| a track coming back within 4–8 m of the gate | 20 / 41 | 28 / 53 |

Against other values of D (the same cases, mild fading):

| D | indoor 1–3 m | outdoor 600 mW | hover (extra per 10 s) |
| --- | --- | --- | --- |
| 10 | 6 / 10 | 4 / 77 | 0.8–1.1 |
| 12 | 9 / 2 | 9 / 36 | 0.4–0.6 |
| **16** | **32 / 0** | **3 / 4** | **0.03** |
| 20 | 67 / 0 | 3 / 2 | 0 |

- **It needs contrast.** A lap is caught when its gate pass stands about D + 2 dB over the
  far side of the track. Put the antenna at the gate (the drone within 1 m), or lower D for
  a small indoor track. A hot link narrows the contrast: the gate pass hits
  the ADC ceiling (54.5–56.7 dB over the floor at gain 40, [host]), so the IPEX antenna
  indoors loses half its laps at 16 dB.
- **A parked drone.** It rings an extra pass when its level swings D and back. At 16 dB
  ([host]):
  - 0.03 per 10 s hover;
  - none for a still drone, crashed or on the pad;
  - about 1 per 30 s on the pad if it wobbles like the bench's hand-held VTX.
- **A track that comes back near the gate** rings there, as the absolute re-arm would if
  the drone fell under the release.
- **Timing.** The relative re-arm changes no pass the absolute re-arm rings: where the
  absolute path works, the passes are the same to the millisecond. In the model, `drop_db`
  3 rings early on a long approach, whichever path armed the channel (D 16, mild: mean
  −298 ms, p95 1.3 s over all laps; −91 ms on the indoor track). Bench B6 retunes
  `drop_db`.
- **The start pad.** A drone that rang as it powered up on the pad stays in COOLDOWN until
  its 1 s average falls 16 dB under its highest level, so its holeshot is lost, as before.

**On the board.**

- `SDR ready … rearm_db=16.0` at boot (the saved D), and `rearm_db=` on `T`'s `CHECKPOINT`
  line. `CHECKPOINT prefs=saved_v3` at boot: a record from before D was saved, read with the
  build's D until the next save.
- `PASS … rearm=rel`: the pass followed a relative re-arm.
- `tools/scan_analyze.py` splits a long rise the same way (`--rearm`, 16 dB by default,
  0 for the old segments), so a recording reads as the board's `PASS` lines do.
- Bench B6 (d) checks it ([bench.md](bench.md#7-first-bench-b0-to-b11-and-results)).

### Setting D

D is a detector parameter like the others ([the table](#parameters-console-k-saved-in-nvs-namespace-checkpoint-key-prefs)):
`K` `e`/`E` (±1 dB, 0 to 40), or a bridge's `CONFIG_SET` (`rearm_db_x10`, link_proto v4;
a value out of range refuses the whole set), saved in the C5's prefs with the rest.
`CONFIG_CHECKPOINT_REARM_DROP_DB` (16) is the first boot's value, and `K` `0`'s and a
version-3 record's.

The degraded C3's page sets it in its *Detector* card (*Re-arm drop*, 0 to 40 dB): its round
"i" button opens a bubble with what it does, the recommended 16 dB and the advice below.
In a fleet the C6 keeps one value for every gate, set in its advanced view (admin), with
the same bubble, and sends it to each gate ([fleet.md](fleet.md), "Re-arm drop (fleet)"):
a gate whose C5 runs another shows it (`D 12`).

A new D applies from the next reading, the channels' states kept:

- a channel in COOLDOWN re-arms once its 1 s average stands the new D under its highest
  level since the pass;
- a channel relatively armed needs its next pass to rise the new D over its trough;
- D set to 0 sends a relatively armed channel back to COOLDOWN: it waits for the release, as
  if the re-arm had always been off. Left relatively armed, its next pass would only have to
  rise over the trough: the next 4 dB wobble rang ([host] `test_pass_detector.c`).

The boot line (`SDR ready … rearm_db=16.0`), the `T` line (`CHECKPOINT … rearm_db=16.0`)
and `K`'s `CHECKPOINT set` line show the value in effect.

Which value ([host], the tables above, mild fading): 16 dB unless the laps say otherwise.

- Laps missed on a small track or with a strong VTX: lower it, 12–14 (indoor, the antenna
  1–3 m from the line: 9 % missed at 12, 32 % at 16).
- Extra laps outdoors: raise it, 18–20 (600 mW: 36 % extra at 12, 4 % at 16, 2 % at 20).
- A lap counts when its gate pass stands about D + 2 dB over the rest of the lap.

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
| `rearm_db`, the relative re-arm drop D ([above](#relative-re-arm), 0 = off) | 16 dB (`CONFIG_CHECKPOINT_REARM_DROP_DB`) | `e`/`E` ±1 dB, 0 to 40 |
| RX gain (forced index) | 40 | `g`/`G` ±2 (relearns the rests) |

The record is version 4 (`PREFS_VERSION`). Version 4 adds D in the two bytes version 3
kept reserved, so a version-3 record is read as it is, every value kept, with the build's
D (`CHECKPOINT prefs=saved_v3` at boot) until the next save writes version 4. Version 3
changed the meter (band power, 10-bit, clipping instead of folding), so the first boot of
sdr_rx ignores a C5VRX-era record and starts on the defaults. The bridges send the same
fields (`CONFIG_SET`, D as `rearm_db_x10`): `alpha` keeps its meaning there.

The defaults are still the C5VRX meter's, tuned on hand passes at 57 Hz (4 out of 4 at
+13 to +16 dB at gain 24, weak bumps reaching +8 dB). They are kept until bench B6
records real passes on this meter, where a 25 mW drone at 0.9 m rises 22–30 dB
(widscan58), so a far 200 mW drone may still pass `min_peak_db`. Retune them then,
together with the C3's and the C6's copies (`firmware/node/main/c3_store.c`,
`firmware/master/main/c6_hub.c`; D's is `LP_REARM_DEFAULT_X10` in `link_proto.h`), in one
commit.

Other keys: `K` then `l` (live rise of R1-R8 and each LO's saturated captures, `CP_TRACE`
lines, 5 a second), `0`
(defaults, D included), `k` (show), `i` (antenna onboard/IPEX, C5-Zero only: GPIO 26, not saved,
then every LO recalibrated on it), the engine benches `s`, `z`, `x`, `n`, `p` and `S`
([bench.md](bench.md#4-engine-bench)), `q` (close). The `K` window stays open 5 s after
each key. At the top level: `T` prints the `FIRMWARE`, `STATUS`, `SDR` and `CHECKPOINT`
lines, `F` recalibrates every LO, `B` recalibrates and verifies the image rejection (VTX
off for both, the gate is blind for about 3 s), `` ` `` reboots into the USB download
mode. The top-level keys are in `components/checkpoint/cp_console.c`, the `K` keys in
`checkpoint.c`.

**Setting `active_db`:** put it below the rise of a drone passing the gate, but above the
rise of the same drone elsewhere on the track. Otherwise only the [relative
re-arm](#relative-re-arm) re-arms the channel between laps, and only if the drone falls
D under its pass.
