# Bench procedure

Hardware: the detector (C5), a VTX on a Raceband channel at fixed power.
Propulsion removed. The detector has no picture and no sound: everything shows
on the console (`python tools/flash.py PORT --monitor-only --log run.log`); the
coin sound is the C6 master's ([fleet.md](fleet.md)).

The receive engine is `components/sdr_rx` ([detector.md](detector.md)). B0 to B4 of
[§7](#7-first-bench-b0-to-b11-and-results) were run on a C5-Zero on 2026-11-15 (results and
gaps in its table). The other expected values are still widscan58's (CapnBry, `PLAN.md` at
`a9fff97`), and the rest of §7 is the bench that settles them. The C5VRX-era results are in
[History](#history).

## 1. Build, flash, startup

```sh
. ~/esp/esp-idf-v6.0.2/export.sh
( cd firmware/detector && idf.py build )            # → build/detector-c5.bin (0 warnings expected)
python tools/flash.py PORT --check
```

On a tree that built the C5VRX engine before, run `tools/fw.sh clean` first: it deletes
every build dir and generated sdkconfig of the `make variants` list
(`firmware/detector/sdkconfig`, `sdkconfig.ipex`, `sdkconfig.xiao-c5`,
`sdkconfig.wave-c5-lcd`, …; never a `*.defaults`). ESP-IDF never reapplies the defaults,
and sdr_rx's CMake stops on a stale one. The CI's PSRAM build (`build-psram`,
`sdkconfig.psram`) is not in that list: delete those by hand if you made them.

Startup prints, in order:

- `FIRMWARE id=… elf_sha256=… engine=sdr_rx chip_rev=v…`.
- `BOOT_TIMING radio_ms=…` once Wi-Fi hosts the PHY (no traffic: `WIFI_MODE_NULL`).
- `CHECKPOINT prefs=saved|defaults gain=40` (a C5VRX-era record reads `defaults`).
- One `SDR cal lo=5677 ms=41 irr_db=37.4 verify_gain=60 tries=1 dc=6 sat=0` per LO: each
  LO is calibrated (~41 ms), then its image rejection is verified from 64 captures at a
  temporary gain of 60, at least 25 dB (`CONFIG_SDR_RX_IRR_MIN_DB`) or up to 3
  calibrations (`tries`).
  `irr_db=skip_saturated` means too many clipped captures: a VTX too close, verify skipped;
  `irr_db=skip_faults` means the captures failed (8 failed captures end a verify try).
- `SDR ready lo=5677,5751,5825,5899 gain=40 verify_tries=3 pair_guard=1 rearm_db=16 cal_ms=…
  boot_ms=… chip_rev=v…` (`verify_tries` is the build's `CONFIG_SDR_RX_VERIFY_TRIES`: 1 only in
  B0's control; `pair_guard` its `CONFIG_CHECKPOINT_PAIR_GUARD`: 0 only in B11's control;
  `rearm_db` its `CONFIG_CHECKPOINT_REARM_DROP_DB`, the relative re-arm), expected
  within about 2 s of the reset; `tools/flash.py --check` waits for it. Passes can ring 3 s
  later, once the rests are learnt.
- `HB t_s=… stage=… live=… rot_hz_x10=… faults=… cal=…` every 5 s, from the USB reader,
  whatever the rest does.

A failed step prints `BOOT failed step=nvs|radio err=…` or `SDR init failed: no detection;
console stays up`; the console and the download key keep working. After `BOOT failed` the
UART link is never started, so the bridges show the gate as absent (no C5 status) and
commands to it fail; only the USB console tells why. After `SDR init failed` the bridges see
*calibration needed* with unknown levels and at least one fault, plus one per failed verify
capture (8 per LO, so 33, when the dump never answers).

`T` prints four lines:

- `FIRMWARE …` again.
- `STATUS uptime_s= heap_internal= heap_largest= heap_min= heap_ok= scan_stack_free=
  console_stack_free= usb_stack_free= link_stack_free= fw=`. `heap_min` is the lowest free
  heap since boot, the Wi-Fi start included: it must stay at 20000 or more. `heap_ok=1`:
  the heap is intact. Note the four `*_stack_free` values (bytes left) before anyone
  trims a stack.
- `SDR plan=pairs lo=… gain=40/83 rot_hz_x10=… rot_us=avg/max tune_us=avg/max cap_us=…
  wait_us=… dsp_us=avg/max faults=… timeout=… short=… overrun=… dsp=… stale=… recal=…
  foreign_dc=… dc_max=… iq_bad=0x… cap_bad=0x… cal_needed=… calibrating=… init_failed=… phy=…`: the
  engine over the last second, and counters since boot. `rot_hz_x10` is the refresh per
  channel × 10. `faults` (capture timeouts, short or overrun dumps, DSP errors) must stay
  0. `stale` counts captures dropped after a PHY-tracking recalibration: not a fault, each
  comes with `foreign_dc` +3. `cal_needed=1`: an LO's DC offset (`dc_max` over 30 counts)
  or its IQ estimate (`iq_bad`, one bit per LO) stayed out of range for 3 s; recalibrate
  (`F`, VTX off). The same when an LO's captures failed on most of its hops for 3 s (`cap_bad`,
  one bit per LO, with `timeout`, `short`, `overrun` or `dsp` climbing); its two channels
  refresh slower or not at all. A channel that gets no reading at all for 1 s shows an
  unknown level on the bridges: every capture of its LO fails, or the pair guard holds it
  while the other channel of its LO clips the ADC ([detector.md](detector.md#pair-ghost-guard)).
- `CHECKPOINT source=sdr live= learning= bench= engine_failed= stalled= stalls= gain= top=
  rise_db= state= rotations= readings= passes= alpha= tau_ms= active_db= …`, and
  `CHECKPOINT last_pass …` once a pass rang.

`tools/flash.py PORT --check` (`--monitor-only --check` on a running board) runs `T` and
ends with `CHECK result=pass|fail …`: it fails on a crash or a reboot (`task_wdt`
included), another image than the one flashed, no answer to `T`, or `faults` other than 0,
and warns on `cal_needed=1`. Its heap warning only fires under 1024 bytes: read `heap_min`
from the `CHECK` line, or pass `--heap-min-warn 20000`.

## 2. Signal

- Send `K` then `l` (trace): `CP_TRACE t_ms=… rise_db=r1,r2,…,r8 top=R… state=… sat=a,b,c,d
  held=h1,…,h8`, 5 lines a second, each channel's rise above its rest in dB. VTX off, every
  rise stays near 0. VTX on and close, its channel rises well above the others (20 dB or
  more at 0.9 m for 25 mW).
- `sat=` counts, per LO in hop order (5677, 5751, 5825, 5899 MHz: R1+R2, R3+R4, R5+R6,
  R7+R8), the captures that hit an ADC rail since the previous line (up to about 100 a line
  per LO at 508 rotations a second, [board] B1). A saturated capture flags both of its
  readings, so this is what the detector sees. A 25 mW VTX clips within about 30 cm of the
  onboard antenna at gain 40 ([board] 2026-11-15).
- `held=` counts, per channel R1 to R8, the readings the pair guard held since the previous
  line ([detector.md](detector.md#pair-ghost-guard)): only the weaker channel of a clipping
  LO, never the strong one.
- **If the levels stay flat with a strong VTX**, report it: the first suspect is the Wi-Fi
  promiscuous filter (`CONFIG_SDR_RX_PROMISC_FILTER_ALL=y`, bench B0).
- Stop the trace (`K` `l`) before the quality measurements.

## 3. Detector

| Test | Expected |
| --- | --- |
| Move the VTX toward the antenna then away, over about 2 s | one `PASS` line, with `delay_ms` < 1000 |
| Same pass, 5 times at least 3 s apart | 5 `PASS` lines |
| VTX held still near the antenna for 30 s | a single `PASS` (or none) |
| VTX off, or far, for 2 min | no `PASS` |
| VTX at medium distance (peak below `min_peak_db`) | no `PASS` |
| Two VTXs on two channels passing together | two `PASS` lines, one per channel |

Tune with `K`: `t`/`T` and `m`/`M` from the measured `peak_db`, then `d`/`D` for
responsiveness ([detector.md](detector.md)).

## 4. Engine bench

`K` then `s`, `z`, `x`, `n` or `p` (`components/checkpoint/cp_bench.c`, console only).
Each bench runs inside the scan task:

- No detection meanwhile: the bridges show *calibrating*, and `STATS` keep flowing.
- While it runs, the C5 refuses a calibration, a gain change, `K` `i` and any other bench
  (`refused=busy`).
- Afterwards the production settings come back by themselves (the PAIRS plan, the PLL
  tune, the IQ correction reset) and the rests are learnt again (3 s). No reset needed.
- Each bench prints a `… start` line, its results and a `… done` line.

The methods are widscan58's. The sample lines below show the formats; their numbers are
examples in widscan58's range, not measurements of this board.

### `K` `s`: rotation rate and the steps of a hop (B1)

VTX off, about 6 s: 2000 rotations with the production PLL-only tune, then 500 with the
full channel setup at every tune.

```
SDRBENCH method=pll rotations=2000 yields=20 rot_us=1890/1961/2410 per_channel_hz_x10=5099 tune_us=210/225/340 cap_us=22/24/30 wait_us=0/1/5 dsp_us=225/237/290 detector_us=8/10/25 faults=0 stale=0
SDRBENCH method=full rotations=500 … tune_us=420/473/710 …
SDRBENCH done
```

The timings are min/median/max. Per hop: `tune_us` tunes the next LO, `cap_us` is the
capture with what is left of its settle wait, `wait_us` that wait alone (the DSP hides the
100 µs settle), `dsp_us` the PSD and the two band levels; `detector_us` is the detector per
reading: the time to feed a hop (the pair guard, then its two readings) over its readings, as
production feeds it since the guard (before it, each reading was timed alone: 11 µs median,
[board] B1). `rot_us` leaves out the rotations that contain the scan task's tick of yield
(`yields=`); `per_channel_hz_x10` is the refresh per channel × 10 from its median.

### `K` `z`: settle after a hop off a strong LO (B2)

VTX on, strong (30 dB or more over the quietest channel), about 5 s. From 200 rotations
it picks the strongest channel S, the quietest Q on another LO and a control C, the
quietest on a third LO. For each delay from 0 to 2000 µs, 8 times: tune S's LO, wait 3 ms,
capture; tune Q's LO, wait the delay, capture 4096 words (51.2 µs) and read Q's level in
each 1024-word quarter (12.8 µs, oldest first); then the same from C's LO, the control.
Then everything again with the full tune.

```
SDRRESIDUE method=pll strong=R1 lo=5677 level_qdb=-96 quiet=R6 lo=5825 control=R8 lo=5899 gain=40 repeats=8
SDRRESIDUE method=pll d_us=0 after_qdb=a/b/c/d control_qdb=a/b/c/d
…
SDRRESIDUE method=pll settle_us=100
SDRRESIDUE method=full …
SDRRESIDUE done
```

Levels are in 0.25 dB codes (dBFS × 4). `settle_us` is the first delay from which every
quarter after the strong LO stays within 1 dB of the control (`none` if it never does).
A `weak` line (S under 30 dB over Q) asks for a stronger VTX; the bench still runs.

### `K` `x`: image rejection between the two channels of an LO (B3)

Two runs, at the same gain.

1. **Every VTX off: the floor run.** 200 rotations; each channel's median is stored as its
   floor: `SDRIMAGE floor stored gain=40 floor_qdb=…` and `SDRIMAGE next: VTX on one channel
   at -10..-5 dBFS, then K x`. The floor is kept until a reboot. A gain change, or no channel
   10 dB over its floor, makes the next `K` `x` a floor run again. A floor run with a channel
   more than 6 dB over the median of the 8 stores nothing (a VTX is on; with every VTX off
   the 8 floors lie within 1 dB of their median, [board] 2026-11-15):
   `SDRIMAGE refused: R6 is 57.0 dB over the channels' median (a floor allows 6 dB), a VTX
   is on, nothing stored: switch every VTX off, then K x`. A floor stored before is kept.
2. **VTX on one channel S**, 50 dB or more over its floor, not clipping (`strong_qdb`
   −40 to −20). Its partner P shares S's LO and S's image lands on it. 200 captures with
   the IQ correction on, 200 with it off (their raw I/Q moments summed too), and 200 with
   P on an LO of its own (the SINGLE placement, a diagnostic).

```
SDRIMAGE strong=R1 partner=R2 lo=5677 gain=40 strong_qdb=-36 partner_on_qdb=-237 partner_off_qdb=-140 floor_qdb=-238 partner_single_qdb=-234 irr_on_db>=59.6 (floor-limited) irr_off_db=26.0 moments_irr_db=27.0 ema_irr_db=38.2
SDRIMAGE done
```

`irr_on_db` and `irr_off_db`: S's level over what the image adds to P's floor, with and
without the correction; `>= X (floor-limited)` when P stays within 0.5 dB of its floor.
`moments_irr_db` comes from the raw I/Q moments, `ema_irr_db` from the correction's own
state. Refusals: `weak` (bring the VTX closer), `clipped` (move it away), no floor stored
for P (VTX off, `K` `x`), and a floor run with a VTX on (run 1).

### `K` `n`: noise against the capture size (B4)

VTX off, about 12 s. 500 captures per LO of 256, 1024 and 4096 words (4, 16 and 64
segments of 64 points), then the borrowed-calibration control.

```
SDRNOISE n=256 segments=4 dsp_us=237 sd_db=1.20/1.10/… mean_qdb=-238/-240/…
SDRNOISE n=1024 …
SDRNOISE n=4096 …
SDRNOISE dc gain=40 calibrated_dc_max=6 threshold=30 borrowed lo=5899 cal_from=5677 dc_i=41 dc_q=-37 mean_qdb=-231/-229
SDRNOISE done
```

`sd_db` is each channel's spread in dB, `mean_qdb` its mean level. The `dc` line tunes
5899 MHz with 5677 MHz's calibration tables (one band edge with the other's: what a stale
calibration looks like) and prints its DC offset next to the calibrated LOs' worst one.
The *calibration needed* threshold (`threshold`, `CONFIG_SDR_RX_DC_CAL_NEEDED`, 30 counts)
must lie between the two (B7). On the C5-Zero at gain 40 the two do not separate ([board]
2026-11-15): `calibrated_dc_max=4`, borrowed `dc_i=8 dc_q=-3`, and the borrowed LO's levels
read within 0.5 dB of the calibrated ones (`mean_qdb=-239/-236` against −237/−234). A stale
calibration is then harmless to the levels, so the threshold stays at 30: it only catches a
gross DC fault.

### `K` `p`: one LO per channel against the pairs

About 1 s, not a toggle and not saved. It prints the PAIRS plan (`SDRPLAN hop=0 lo=5677
R1=-19.0MHz/bins13..20 R2=+18.0MHz/bins43..50`, …), takes 200 rotations, switches to
SINGLE (one LO per channel, 8 MHz below it; its 8 new LOs calibrate on their first visit),
prints that plan, takes 100 rotations, then `SDRPLAN compare gain=… pairs_qdb=… single_qdb=…
pairs_hz_x10=… single_hz_x10=…` and `SDRPLAN done (PAIRS again)`. `pairs_qdb` is also the
quickest read of every channel's median level at the current gain. On the C5-Zero, a 25 mW
VTX on R6 ([board] 2026-11-15): PAIRS 537 Hz per channel with R6 at +41 dB, SINGLE 269 Hz
with R6 at +34 dB. PAIRS stays the plan: twice the rate, and the VTX reads at least as strong.

## 5. Scan recording: do real passes stand out?

This records every channel's level, to find the passes offline and tune the detector.
`K` then `S` (`cp_bench.c`); a bench like the others: no detection while it runs.

1. Log the console to a file (`python tools/flash.py PORT --monitor-only --log run.log`).
   Set a VTX on a known channel, with the power low enough that it does not clip up close.
2. `K` then `S` starts it: `SCANBENCH start decim=15 row_ms=29.5 rows=2048 keep_s=60
   max_s=3600 order=1+2:3+4:5+6:7+8 gain=40: …`. A row is the mean of `decim` rotations,
   per channel, in 0.25 dB codes, plus each LO's saturated captures among those rotations.
   The ring keeps the newest 60 s at least: it fills the bench scratch (44 KiB in the RF dump
   bank), 2048 rows of 22 bytes, and `decim` is the rotations a second × 60 / 2048, rounded
   up (15 at the board's 507.9, [board] B1).
3. Move the VTX past the antenna a few times, a second or so each, a few seconds apart.
4. `K` then `S` again stops it, **within 60 s of the first pass**. The dump follows:

```
SCANDUMP begin rotations=… kept=… kept_s=… decim=15 format=qdb order=1+2:3+4:5+6:7+8 gain=40 rot_us=… fw=…
SCAN,<t_us>,<R1>,<R2>,…,<R8>,<sat>
…
SCANDUMP end rotations=… decim=15 format=qdb order=… gain=40
SCANBENCH done stop=key rotations=… rows=… faults=…
```

5. Read the recording:

```sh
python3 tools/scan_analyze.py run.log
```

`order=` lists the hops in reading order, `a+b` for two channels of one capture. A level
is the code / 4 in dBFS, −32768 when the row had no reading. `<sat>` is 4 hex digits, one
per hop in that order (`0f00`: R3+R4's LO clipped in 15 of the row's rotations): the
captures of that LO that hit an ADC rail, 15 at most (`f` is 15 or more when `decim` is
over 15). Recordings from before it end at R8; `scan_analyze.py` reads both, and the
C5VRX-era power format too. It prints the real refresh, each channel's rest, median and
peak, and the passes: a channel rising 8 dB (`--margin`) above its own rest, smoothed like
the detector (`--alpha`, per 17.5 ms), timed at the centroid of the peak. A rise that stays
up is split like the detector's relative re-arm (`--rearm`, 16 dB; `0` for one pass per
rise, as before). A pass seen on
several channels at once goes to the strongest, and the others are listed as `also on`.
With `<sat>`, each pass ends with the saturated share of every LO over its span
(`saturated 1+2 0% 3+4 0% 5+6 41% 7+8 0%`): a rise on R5 during a pass of R6 that comes
with saturated captures on 5+6 is the clipping ghost (the rails' products land in the
partner's window).

What decides: every pass found, on the right channel, none invented, and neighbours well
below it. The partner channel of the pass's LO is the one to watch (its image, §4 `K` `x`).
For endurance, leave it running (one hour at most): a `SCANBENCH live t_s=… rotations=…
per_channel_hz_x10=… faults=… peak_qdb=… kept_s=…` line every 10 s gives the refresh, the
faults and each channel's peak; then do a few passes and stop it. The refresh must not sag
and passes must still come out.

## 6. Gain and antenna

**Gain.** `K` then `g` or `G` steps the forced RX gain by 2, between 10 and the PHY's
maximum (`gain=40/83` on `T`); it is saved and the rests are learnt again. Above about 25,
one index is about 1 dB, and with the onboard antenna clipping starts near 62
(widscan58). The default 40 is widscan58's preset for this board: the weakest channel
about 22 dB over its floor at 0.9 m for 25 mW, with room left for a 200 mW drone passing
close. The C5VRX meter's 24 means nothing here.

**Antenna.** The C5-Zero's antenna switch is GPIO 26, high for the IPEX connector, low for
the onboard antenna, and the firmware leaves it alone. `K` then `i` switches it at run
time (C5-Zero only, not saved, back to onboard after a reset), prints `CHECKPOINT
antenna=ipex (GPIO 26 high) recal=quick`, recalibrates every LO on that antenna and learns
the rests again. `CONFIG_CHECKPOINT_ANTENNA_IPEX=y` (the `detector-ipex` build) boots on
the IPEX connector, set before the radio starts so the boot calibration sees that antenna.
A boot with nothing on the IPEX connector is deaf.

### For later: hardware attenuation, for a more precise pass point

Not needed yet: try the gain first, it is free and set per gate. Come back to this
when a gate needs to know more precisely where and when a drone passed:

- **A directive or shielded antenna aimed at the gate** is the real gain: it narrows the zone
  the C5 sees, so the pass peak gets sharper and the timing better. `hardware/antenna/` holds
  a first printed design, not simulated or built yet.
- **A fixed attenuator** (20 dB, on an IPEX to SMA lead, rated to 6 GHz: many stop lower) only
  shifts the levels down: useful if the RF stage saturates before the gain setting can help.

Too much attenuation misses a drone passing high or far from the antenna, and every pilot
must fly the same power (25 mW and 600 mW are 14 dB apart).

## 7. First bench (B0 to B11) and results

Setup: the detector on USB, logged (`python tools/flash.py PORT --monitor-only --log
run.log`); a 25 mW VTX on a known channel at 0.9 m; the onboard antenna. Before B0, run
`tools/fw.sh clean` (§1).
Report the `SDR cal`, `SDRBENCH`, `SDRRESIDUE`, `SDRIMAGE` and `SDRNOISE` lines and the
`scan_analyze.py` output, and fill the tables below.

**B0, boot.** Flash, reset 5 times, `T` each time.
- Pass: `SDR ready` within 2.0 s of the reset (0.5 s more if a verify retried); 4 `SDR cal`
  lines with `irr_db` 25 or more and `tries` 3 or less; on `T`: `faults=0`, `heap_min` 20000
  or more, `heap_ok=1`, `cal_needed=0`; no crash or `task_wdt` in 10 min; VTX on, its
  channel 20 dB or more over the others. Record the four `*_stack_free`.
- Then the verify's positive control, in a build dir and sdkconfig of its own, so the
  production sdkconfig never holds it (ESP-IDF keeps a value an existing sdkconfig already
  has, and `flash.py`'s drift check only compares what a defaults file sets): put
  `CONFIG_SDR_RX_VERIFY_TRIES=1` in a scratch file, say `/tmp/verify1.defaults`, then
  `( cd firmware/detector && idf.py -B build-verify1 -DSDKCONFIG=sdkconfig.verify1
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;/tmp/verify1.defaults" build )` and `python
  tools/flash.py PORT --build firmware/detector/build-verify1`. The boot prints `SDR ready
  … verify_tries=1`: the first calibration is kept whatever it reads. Reboot 10 times; after
  each, `K` `x` on R1 (the floor run, then the VTX on R1), and record the pair
  (the boot `irr_db` of LO 5677, `K` `x`'s `irr_off_db`). Pass: every pair within 5 dB, and
  every `irr_off_db` under 25 with an `irr_db` under 25. widscan58 saw about half of its
  first calibrations bad, so some pairs should be. Afterwards reflash the normal build
  (`python tools/flash.py PORT`; its boot prints `verify_tries=3`) and delete
  `firmware/detector/build-verify1` and `firmware/detector/sdkconfig.verify1`.
- If not: no `SDR ready`, send the `SDR init` or `BOOT failed` lines. Levels flat with
  the VTX on: set `CONFIG_SDR_RX_PROMISC_FILTER_ALL=y`. Control failed: report the pairs;
  the fallback is `CONFIG_SDR_RX_VERIFY_TRIES=1` in `sdkconfig.defaults` (the verify then
  only logs; run `tools/fw.sh clean` and rebuild).

**B1, rate.** `K` `s`, VTX off.
- Pass: PLL `per_channel_hz_x10` 4000 or more (target about 5000), `tune_us` median 300 or
  less, `cap_us` 35 or less, `dsp_us` median 300 or less, `faults=0`; FULL `tune_us` 400 to
  750 (a sanity check).
- If under 400 Hz: report which step is slow (a slow tune: libphy not in IRAM; a slow DSP:
  `-O2` not applied).

**B2, settle.** VTX on, strong (30 dB or more), `K` `z`.
- Pass: `settle_us` 100 or less for `pll`: keep `CONFIG_SDR_RX_SETTLE_US=100`.
- Between 100 and 300: set `CONFIG_SDR_RX_SETTLE_US` to it (the DSP hides about 240 µs of
  it, each hop costs the rest). 800 or more (the C5VRX engine's 1 ms tail): stop and
  report; the rate goal would then need another tune method.

**B3, image.** Every VTX off: `K` `x` (the floor run, `SDRIMAGE floor stored`). Then the
VTX on R1, then R2, R7 and R8, each brought to −10 to −5 dBFS (`strong_qdb` −40 to −20, no
clipping): `K` `x` each time.
- Pass: `irr_on_db` 45 or more on every pair, or `>= X (floor-limited)` with X 45 or more
  (widscan58: 55–58); `moments_irr_db` within 5 dB of `irr_off_db`.
- If not: `weak` or `clipped`, move the VTX. A floor run refused with every VTX off: report
  the line (a board spur on that channel?). Under 45, report the raw numbers.

**B4, noise.** VTX off, `K` `n`.
- Pass: each `sd_db` within 30 % of 1.06–1.32 dB (256 words), 0.55–0.68 (1024) and
  0.28–0.40 (4096); the means of the three sizes within 0.5 dB of each other.
- If larger: check the gain (40) and the floor (about −60 dBFS, −240 in codes).

**B5, channels and gain.** The VTX on R1, then each channel up to R8, at 0.9 m; then on R4
at gains 30 to 50 in steps of 4 (`K` `G` twice a step), reading R4's level at each from
`K` `p`'s `pairs_qdb`.
- Pass: every channel rises 20 dB or more over its rest (`K` `l`); R8 within 3 dB of R7;
  about 1 dB per gain index above 25.
- If a channel is dead: report its channel and LO.

**B6, passes.**
- (a) `K` `S`, 10 hand passes on 2 channels **within 60 s**, `K` `S`; then
  `python3 tools/scan_analyze.py run.log`. Pass: 10 out of 10 found.
- (b) Live, no recording: the same 10 passes, then 2 min idle, then a steady VTX for 30 s,
  reading the `PASS` lines; then the tests of §3. Pass: one `PASS` per pass, none idle,
  one at most for the steady VTX.
- (c) **Far drones**, live: a VTX at 3, 6 and 10 m from the gate, at 25 and 200 mW, on 2
  channels, flown or walked past at each distance. Record each pass's `peak_db` in the
  table below. Pass: only the passes within the gate's radius ring.
- (d) **Relative re-arm**, live: a 25 mW VTX held about 3 m away (`K` `l`: note its rise),
  then 5 hand passes at 0.3 m, back to 3 m for 5–10 s after each; then walk it 10 m away
  and back to 0.3 m, 3 times. Pass: one `PASS` per pass, `rearm=rel` on each after the
  first, none while it is held at 3 m. Record each pass's `peak_db` and the rise at 3 m:
  the drop between them is what the default (16 dB, [detector.md](detector.md#relative-re-arm))
  must stay under.
- If not: tune `min_peak_db` and `active_db` from the recorded rises until (c) holds, then
  change the C3 and C6 defaults with them ([detector.md](detector.md)). A fleet waits for
  (c) before using this engine.

**B7, long run.** One hour with the C3 web page open (`STATS`), at room temperature, then
10 min of warming the board (a hair dryer), then `K` `n`.
- Pass: `faults` 0 (or under 1 per 10⁵ hops); `rot_hz_x10` steady within 5 %; `heap_min`
  stable; `cal_needed=0` at room temperature. Warmed: `foreign_dc` steps by 3 for each PHY
  tracking recalibration, `stale` steps with it, no reading of that LO jumps (no unforced
  capture, about +27 dB, is used), and the gate recovers within 1 s (a gap of about 0.2 s
  per channel). DC control: `K` `n`'s borrowed DC (`dc_i` or `dc_q`, whichever is
  larger in magnitude) above `threshold`, and `calibrated_dc_max` under it.
- If gaps or hangs come with a tracking recalibration: set
  `CONFIG_ESP_PHY_DISABLE_PLL_TRACK=y` and report. If the threshold is not between the two
  DC values: keep 30 while the borrowed levels stay within 1 dB of the calibrated ones (the
  case at gain 40, §4 `K` `n`); otherwise set `CONFIG_SDR_RX_DC_CAL_NEEDED` midway (on a log
  scale) and record it.

**B8, XIAO.** `tools/fw.sh build detector-xiao-c5`, flash it on a XIAO ESP32-C5 (`python
tools/flash.py PORT --build firmware/detector/build-xiao-c5`), then B0 and B1. Pass: as B0
and B1, with `faults` 0 over 10 min (the PSRAM and the dump bank coexist).

**B9, IPEX.** `tools/fw.sh build detector-ipex`, flashed on a C5-Zero with an antenna on
its IPEX connector (`python tools/flash.py PORT --build firmware/detector/build-ipex`);
`K` `i` twice. Pass: the boot prints `CHECKPOINT antenna=ipex`, each `K` `i`
prints `recal=quick`, and the levels change as expected.

**TODO (not run yet, deferred on 2026-11-15).** Bench the IPEX connector with an external
antenna: B9 itself, then the clipping headroom. On the onboard antenna at gain 40, a 25 mW
VTX clips the ADC within about 30 cm, and the clipping puts a ghost on the other channel of
its LO. So redo on IPEX a `K` `S` recording with passes close to the antenna and B6 (c), and
decide from them whether the IPEX lead needs a fixed attenuator (a PI pad, see
[For later: hardware attenuation](#for-later-hardware-attenuation-for-a-more-precise-pass-point)).

**B10, link.** From the C3's web page: *Cal F*, *Cal B*, *Reboot*. Then the download key
(`` ` ``) while the detector scans, 5 times: `python tools/flash.py PORT` sends it through
`tools/enter_download.py`, then flashes.
- Pass: the *calibrating* badge for 4 s at most (F) or 5 s (B), then OK; the VTX guard
  refuses with a VTX on; *Reboot* prints `SDR reboot=requested` and `STATS` come back within
  5 s; the download key enters the ROM loader every time (`enter_download.py` sees the
  acknowledgement, esptool connects).
- If the download key fails: report it; the suspect is the engine's quiesce before the
  restart.

**B11, pair ghost guard.** The guard ([detector.md](detector.md#pair-ghost-guard)) keys on
the saturated flag only, and its constants come from a host model: these steps show
whether the board behaves as the model assumes. A 25 mW VTX on R6 (25 mW at a distance d
clips like 600 mW at 4.9 d); the production build, from a `tools/fw.sh clean` tree (the
boot prints `pair_guard=1`), logged (`flash.py --log`). In this order:

1. **B-g4, link anchor.** R6 held still at 3.0, 1.0, 0.5, 0.3, 0.2, 0.15, 0.10 and
   0.06 m, `K` `p` three times at each; note R6's and R5's `pairs_qdb`. The host model
   expects about −78 qdb at 0.9 m (its 40 dB anchor) and the first clipped captures from
   0.175 m.
2. **B-g2, compression before the rails.** Store a floor first (every VTX off, `K` `x`).
   Then the same steps from 0.3 to 0.06 m: at each, `K` `l` for a few lines (R5's rise,
   `sat=`), then `K` `x` on R6 (`irr_on_db`, `partner_on_qdb`), until it refuses `clipped`.
   If R5 rises 3 dB or more while `sat=`'s third field (LO 5825) still reads 0, the
   receiver compresses before the rails: the guard misses that, report it.
3. **B-g1, the flag.** `K` `l` on: 10 swipes at 6 cm, 10 passes at 1 m/s at 6 cm, 10 at
   15 cm. Pass: every rise of R5 comes with saturated captures on LO 5825 (`sat=`), R5's
   `held=` count rises, no R5 `PASS`, and 30 R6 `PASS` lines of 30.
4. **B-g3, recording.** `K` `S`: 3 swipes and 3 hand-speed passes at 6 cm, a 5 s hold at
   6 cm, 3 passes at 30 cm; stop within 60 s; `python3 tools/scan_analyze.py run.log`.
   Note each pass's `saturated` share: R5's rises should sit in rows where 5+6 clipped.
5. **B-g5, a real partner.** A second VTX on R5 held still at 0.6 m while R6 swipes at
   6 cm: R5 must not `PASS`, and its rise comes back within about 100 ms of the last
   saturated capture. Then move both together, 10 times. Compare the `PASS` count and
   `peak_ms` with a guard-off build, made like B0's control: `CONFIG_CHECKPOINT_PAIR_GUARD=n`
   in `/tmp/guard0.defaults`, `( cd firmware/detector && idf.py -B build-guard0
   -DSDKCONFIG=sdkconfig.guard0 "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;/tmp/guard0.defaults"
   build )`, `python tools/flash.py PORT --build firmware/detector/build-guard0` (its boot
   prints `pair_guard=0`). Afterwards reflash the normal build and delete
   `firmware/detector/build-guard0` and `firmware/detector/sdkconfig.guard0`.
6. **B-g6, other LOs.** R6 held at 6 cm, `K` `p` during the hold. A rise over 3 dB on R7,
   R8 or R1–R4 is a leak the guard does not cover (`K` `S` recordings already show +4 to
   +7 dB at most on the other LOs with R6 at the ceiling, [board] 2026-11-15).
7. **B-g7, hover.** R6 held 10 s at 6 cm while R5 passes at 1 m every 3 s. Expect no R5
   lap, no phantom, and R5 *unknown* in `STATS` (the C3's page) during the hold.

- Retune the constants (`SD_HOLD_*` in `scan_detector.h`) from these numbers, not from the
  host model. Missed rises (B-g2) or leaks (B-g6) need another guard: report them.

| Step | Date | Board, firmware id | Result | Numbers |
| --- | --- | --- | --- | --- |
| B0 boot | 2026-11-15 | C5-Zero, onboard antenna, `5.0.0-dev+34259ce5b8b7` | Partial: `heap_min`, the four `*_stack_free` and the 10 min run not done yet | `boot_ms` 982–1034, `faults=0`; `irr_db` 31.5/28.7/27.7/34.9 and 31.8/26.5/30.7/31.7 (two boots), `tries` up to 3 (the threshold was 30 then, 25 since) |
| B0 control | | | Not run yet | 10 pairs (`irr_db`, `irr_off_db`) |
| B1 rate | 2026-11-15 | as B0 | Pass | pll: `per_channel_hz_x10=5079` (507.9 Hz), rotation 1957 µs, `tune_us` 216, `cap_us` 29, `dsp_us` 216, detector 11 µs; full: `tune_us` 438 |
| B2 settle | 2026-11-15 | as B0 | Pass | `settle_us` 0 (pll), 0 (full); no residue across LOs |
| B3 image | 2026-11-15 | as B0 | Effectively pass, on R6/R5 only, from `K` `S`; `K` `x` not measured | Every floor run had the VTX on (R6's floor −8, −41, −30, −26 qdb; refused since, §4). `K` `S`: R6 at −20 to −7.5 dBFS, R5 (its partner) +0.5 dB median over its rest: rejection ≥ ~49 dB, floor-limited |
| B4 noise | 2026-11-15 | as B0 | Pass | `sd_db` 1.02–1.08 (256), 0.50–0.58 (1024), 0.26–0.29 (4096), means within 0.25 dB; `calibrated_dc_max=4`, borrowed `dc_i=8`/`dc_q=-3`: not separable, kept 30 (§4 `K` `n`) |
| B5 channels | | | | rise per channel; R4 level per gain |
| B6 passes | | | | (a) found/10, (b) `PASS` counts, (d) `PASS`/8, `rearm=`, the rise at 3 m and `peak_db` |
| B7 long run | | | | `faults`, `rot_hz_x10`, `heap_min`, `foreign_dc`, `stale` |
| B8 XIAO | | | | |
| B9 IPEX | | | TODO, deferred (see B9) | |
| B10 link | | | | badge times, download key 5/5 |
| B11 pair guard | | | | B-g4 R6/R5 `pairs_qdb` per distance; B-g2 R5's rise before the first saturated capture; B-g1 R5 `PASS`/30, R6 `PASS`/30; B-g3 saturated share per pass; B-g5 `PASS` counts and `peak_ms`, guard on/off; B-g6 worst rise on the other LOs; B-g7 R5 laps, phantoms |

B6 (c), far drones (`peak_db` of each pass, `—` if none rang):

| Distance | 25 mW, channel A | 25 mW, channel B | 200 mW, channel A | 200 mW, channel B |
| --- | --- | --- | --- | --- |
| 3 m | | | | |
| 6 m | | | | |
| 10 m | | | | |

## History

Before issue #2 the detector ran on the C5VRX-5 engine, read one channel at a time and
reached about 57 Hz per channel. Its benches are in this page as it stood on `main` at
`e7d40c9` (`git show e7d40c9:docs/bench.md`,
[on GitHub](https://github.com/defane/FPV-5.8-Checkoint/blob/e7d40c9/docs/bench.md)):
the hop bench of [issue #1](https://github.com/defane/FPV-5.8-Checkoint/issues/1) (about
88 Hz per channel with the `force` hop), the residue that lingered about 1 ms after a hop
and took the scan to 57 Hz, the gain-24 scan recordings (4 passes out of 4 on R4 at +13 to
+16 dB), and the IPEX check (`K` `i`: unplugging the IPEX antenna moved the levels by
1–2 dB on the onboard antenna and by over 30 dB on the IPEX one, 2026-11-05).
