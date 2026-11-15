# Bench procedure

Hardware: the detector (C5), a VTX on a Raceband channel at fixed power.
Propulsion removed. The detector has no picture and no sound: everything shows
on the console (`python tools/flash.py PORT --monitor-only --log run.log`); the
coin sound is the C6 master's ([fleet.md](fleet.md)).

The receive engine is `components/sdr_rx` ([detector.md](detector.md)). Nothing below
has been measured on this board yet: the expected values are widscan58's (CapnBry,
`PLAN.md` at `a9fff97`), and [§7](#7-first-bench-b0-to-b10-and-results) is the bench that
settles them. The C5VRX-era results are in [History](#history).

## 1. Build, flash, startup

```sh
. ~/esp/esp-idf-v6.0.2/export.sh
( cd firmware/detector && idf.py build )            # → build/detector-c5.bin (0 warnings expected)
python tools/flash.py PORT --check
```

On a tree that built the C5VRX engine before, run `tools/fw.sh clean` and delete the
generated `firmware/detector/sdkconfig` (and `sdkconfig.ipex`, `sdkconfig.xiao-c5`,
`sdkconfig.wave-c5-lcd`, `sdkconfig.psram`; never the `*.defaults`) first: ESP-IDF never
reapplies the defaults, and sdr_rx's CMake stops on a stale one.

Startup prints, in order:

- `FIRMWARE id=… elf_sha256=… engine=sdr_rx chip_rev=v…`.
- `BOOT_TIMING radio_ms=…` once Wi-Fi hosts the PHY (no traffic: `WIFI_MODE_NULL`).
- `CHECKPOINT prefs=saved|defaults gain=40` (a C5VRX-era record reads `defaults`).
- One `SDR cal lo=5677 ms=41 irr_db=37.4 verify_gain=60 tries=1 dc=6 sat=0` per LO: each
  LO is calibrated (~41 ms), then its image rejection is verified from 64 captures at a
  temporary gain of 60, at least 30 dB or up to 3 calibrations (`tries`).
  `irr_db=skip_saturated` means too many clipped captures: a VTX too close, verify skipped.
- `SDR ready lo=5677,5751,5825,5899 gain=40 cal_ms=… boot_ms=… chip_rev=v…`, expected within
  about 2 s of the reset; `tools/flash.py --check` waits for it. Passes can ring 3 s later,
  once the rests are learnt.
- `HB t_s=… stage=… live=… rot_hz_x10=… faults=… cal=…` every 5 s, from the USB reader,
  whatever the rest does.

A failed step prints `BOOT failed step=nvs|radio err=…` or `SDR init failed: no detection;
console stays up`; the console and the download key keep working, and the bridges see
*calibration needed* and one fault.

`T` prints four lines:

- `FIRMWARE …` again.
- `STATUS uptime_s= heap_internal= heap_largest= heap_min= heap_ok= scan_stack_free=
  console_stack_free= usb_stack_free= link_stack_free= fw=`. `heap_min` is the lowest free
  heap since boot, the Wi-Fi start included: it must stay at 20000 or more. `heap_ok=1`:
  the heap is intact. Note the four `*_stack_free` values (bytes left) before anyone
  trims a stack.
- `SDR plan=pairs lo=… gain=40/83 rot_hz_x10=… rot_us=avg/max tune_us=avg/max cap_us=…
  wait_us=… dsp_us=avg/max faults=… timeout=… short=… overrun=… dsp=… stale=… recal=…
  foreign_dc=… dc_max=… iq_bad=0x… cal_needed=… calibrating=… init_failed=… phy=…`: the
  engine over the last second, and counters since boot. `rot_hz_x10` is the refresh per
  channel × 10. `faults` (capture timeouts, short or overrun dumps, DSP errors) must stay
  0. `stale` counts captures dropped after a PHY-tracking recalibration: not a fault, each
  comes with `foreign_dc` +3. `cal_needed=1`: an LO's DC offset (`dc_max` over 30 counts)
  or its IQ estimate stayed out of range for 3 s; recalibrate (`F`, VTX off).
- `CHECKPOINT source=sdr live= learning= bench= engine_failed= stalled= stalls= gain= top=
  rise_db= state= rotations= readings= passes= alpha= tau_ms= active_db= …`, and
  `CHECKPOINT last_pass …` once a pass rang.

`tools/flash.py PORT --check` (`--monitor-only --check` on a running board) runs `T` and
ends with `CHECK result=pass|fail …`: it fails on a crash or a reboot (`task_wdt`
included), another image than the one flashed, no answer to `T`, or `faults` other than 0,
and warns on `cal_needed=1`. Its heap warning only fires under 1024 bytes: read `heap_min`
from the `CHECK` line, or pass `--heap-min-warn 20000`.

## 2. Signal

- Send `K` then `l` (trace): `CP_TRACE t_ms=… rise_db=r1,r2,…,r8 top=R… state=…`, 5 lines a
  second, each channel's rise above its rest in dB. VTX off, every rise stays near 0. VTX
  on and close, its channel rises well above the others (20 dB or more at 0.9 m for
  25 mW).
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
reading. `rot_us` leaves out the rotations that contain the scan task's tick of yield
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

1. **VTX off: the floor run.** 200 rotations; each channel's median is stored as its floor:
   `SDRIMAGE floor stored gain=40 floor_qdb=…` and `SDRIMAGE next: VTX on one channel at
   -10..-5 dBFS, then K x`. The floor is kept until a reboot. A gain change, or no channel
   10 dB over its floor, makes the next `K` `x` a floor run again.
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
for P (VTX off, `K` `x`).

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
must lie between the two (B7).

### `K` `p`: one LO per channel against the pairs

About 1 s, not a toggle and not saved. It prints the PAIRS plan (`SDRPLAN hop=0 lo=5677
R1=-19.0MHz/bins13..20 R2=+18.0MHz/bins43..50`, …), takes 200 rotations, switches to
SINGLE (one LO per channel, 8 MHz below it; its 8 new LOs calibrate on their first visit),
prints that plan, takes 100 rotations, then `SDRPLAN compare gain=… pairs_qdb=… single_qdb=…
pairs_hz_x10=… single_hz_x10=…` and `SDRPLAN done (PAIRS again)`. `pairs_qdb` is also the
quickest read of every channel's median level at the current gain.

## 5. Scan recording: do real passes stand out?

This records every channel's level, to find the passes offline and tune the detector.
`K` then `S` (`cp_bench.c`); a bench like the others: no detection while it runs.

1. Log the console to a file (`python tools/flash.py PORT --monitor-only --log run.log`).
   Set a VTX on a known channel, with the power low enough that it does not clip up close.
2. `K` then `S` starts it: `SCANBENCH start decim=14 row_ms=27.5 rows=2252 keep_s=60
   max_s=3600 order=1+2:3+4:5+6:7+8 gain=40: …`. A row is the mean of `decim` rotations,
   per channel, in 0.25 dB codes; the ring keeps the newest 60 s at least.
3. Move the VTX past the antenna a few times, a second or so each, a few seconds apart.
4. `K` then `S` again stops it, **within 60 s of the first pass**. The dump follows:

```
SCANDUMP begin rotations=… kept=… kept_s=… decim=14 format=qdb order=1+2:3+4:5+6:7+8 gain=40 rot_us=… fw=…
SCAN,<t_us>,<R1>,<R2>,…,<R8>
…
SCANDUMP end rotations=… decim=14 format=qdb order=… gain=40
SCANBENCH done stop=key rotations=… rows=… faults=…
```

5. Read the recording:

```sh
python3 tools/scan_analyze.py run.log
```

`order=` lists the hops in reading order, `a+b` for two channels of one capture. A level
is the code / 4 in dBFS, −32768 when the row had no reading. `scan_analyze.py` also reads
the C5VRX-era power format. It prints the real refresh, each channel's rest, median and
peak, and the passes: a channel rising 8 dB (`--margin`) above its own rest, smoothed like
the detector (`--alpha`, per 17.5 ms), timed at the centroid of the peak. A pass seen on
several channels at once goes to the strongest, and the others are listed as `also on`.

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

## 7. First bench (B0 to B10) and results

Setup: the detector on USB, logged (`python tools/flash.py PORT --monitor-only --log
run.log`); a 25 mW VTX on a known channel at 0.9 m; the onboard antenna. Before B0, run
`tools/fw.sh clean` and delete the generated sdkconfig files listed in §1.
Report the `SDR cal`, `SDRBENCH`, `SDRRESIDUE`, `SDRIMAGE` and `SDRNOISE` lines and the
`scan_analyze.py` output, and fill the tables below.

**B0, boot.** Flash, reset 5 times, `T` each time.
- Pass: `SDR ready` within 2.0 s of the reset (0.5 s more if a verify retried); 4 `SDR cal`
  lines with `irr_db` 30 or more and `tries` 3 or less; on `T`: `faults=0`, `heap_min` 20000
  or more, `heap_ok=1`, `cal_needed=0`; no crash or `task_wdt` in 10 min; VTX on, its
  channel 20 dB or more over the others. Record the four `*_stack_free`.
- Then the verify's positive control. Build with `CONFIG_SDR_RX_VERIFY_TRIES=1` (that line
  in a `sdkconfig.local.defaults` next to `sdkconfig.defaults`, the generated sdkconfig
  deleted, `tools/fw.sh build detector`, `python tools/flash.py PORT`): the first
  calibration is kept whatever it reads. Reboot 10 times; after each, `K` `x` on R1 (the
  floor run, then the VTX on R1), and record the pair
  (the boot `irr_db` of LO 5677, `K` `x`'s `irr_off_db`). Pass: every pair within 5 dB, and
  every `irr_off_db` under 30 with an `irr_db` under 30. widscan58 saw about half of its
  first calibrations bad, so some pairs should be. Remove the line afterwards.
- If not: no `SDR ready`, send the `SDR init` or `BOOT failed` lines. Levels flat with
  the VTX on: set `CONFIG_SDR_RX_PROMISC_FILTER_ALL=y`. Control failed: keep
  `CONFIG_SDR_RX_VERIFY_TRIES=1` (the verify then only logs) and report the pairs.

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

**B3, image.** VTX off: `K` `x` (the floor run). Then the VTX on R1, then R2, R7 and R8,
each brought to −10 to −5 dBFS (`strong_qdb` −40 to −20, no clipping): `K` `x` each time.
- Pass: `irr_on_db` 45 or more on every pair, or `>= X (floor-limited)` with X 45 or more
  (widscan58: 55–58); `moments_irr_db` within 5 dB of `irr_off_db`.
- If not: `weak` or `clipped`, move the VTX. Under 45, report the raw numbers.

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
  DC values: set `CONFIG_SDR_RX_DC_CAL_NEEDED` midway (on a log scale) and record it.

**B8, XIAO.** `tools/fw.sh build detector-xiao-c5`, flash it on a XIAO ESP32-C5 (`python
tools/flash.py PORT --build firmware/detector/build-xiao-c5`), then B0 and B1. Pass: as B0
and B1, with `faults` 0 over 10 min (the PSRAM and the dump bank coexist).

**B9, IPEX.** `tools/fw.sh build detector-ipex`, flashed on a C5-Zero with an antenna on
its IPEX connector (`python tools/flash.py PORT --build firmware/detector/build-ipex`);
`K` `i` twice. Pass: the boot prints `CHECKPOINT antenna=ipex`, each `K` `i`
prints `recal=quick`, and the levels change as expected.

**B10, link.** From the C3's web page: *Cal F*, *Cal B*, *Reboot*. Then the download key
(`` ` ``) while the detector scans, 5 times: `python tools/flash.py PORT` sends it through
`tools/enter_download.py`, then flashes.
- Pass: the *calibrating* badge for 4 s at most (F) or 5 s (B), then OK; the VTX guard
  refuses with a VTX on; *Reboot* prints `SDR reboot=requested` and `STATS` come back within
  5 s; the download key enters the ROM loader every time (`enter_download.py` sees the
  acknowledgement, esptool connects).
- If the download key fails: report it; the suspect is the engine's quiesce before the
  restart.

| Step | Date | Board, firmware id | Result | Numbers |
| --- | --- | --- | --- | --- |
| B0 boot | | | | `boot_ms`, `irr_db` ×4, `tries`, `heap_min`, the four `*_stack_free` |
| B0 control | | | | 10 pairs (`irr_db`, `irr_off_db`) |
| B1 rate | | | | `per_channel_hz_x10`, `tune_us`, `cap_us`, `dsp_us` (pll, full) |
| B2 settle | | | | `settle_us` (pll, full) |
| B3 image | | | | `irr_on_db`, `irr_off_db`, `moments_irr_db` for R1, R2, R7, R8 |
| B4 noise | | | | `sd_db` per size; `calibrated_dc_max`, borrowed `dc_i`/`dc_q` |
| B5 channels | | | | rise per channel; R4 level per gain |
| B6 passes | | | | (a) found/10, (b) `PASS` counts |
| B7 long run | | | | `faults`, `rot_hz_x10`, `heap_min`, `foreign_dc`, `stale` |
| B8 XIAO | | | | |
| B9 IPEX | | | | |
| B10 link | | | | badge times, download key 5/5 |

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
