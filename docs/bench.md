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

## 4. Hop bench: can one C5 scan Raceband?

Decides [issue #1](https://github.com/defane/FPV-5.8-Checkoint/issues/1): how fast the
C5 can hop between R1 and R8, and whether a channel still reads correctly after a
fast hop. It is `components/checkpoint/hop_bench.c`, on the console only.

Setup: the detector on its USB console, logged
(`python tools/flash.py PORT --monitor-only --log run.log`), and a VTX at fixed power
on a known channel, say R3, at a steady distance. It should not clip: `clip_pm` near 0
in the output, otherwise move it away.

Run it twice, with the same setup: VTX off, then VTX on. Each run is `K` then `s`:
no detection for about 15 s, then `HOPBENCH` lines. **Reset the board after the last
run**, before any other measurement: the channel, offset and gain are restored, but the
AFC and the video standard state are not.

It measures seven ways of hopping, each 4 sweeps over R1-R8 from across the band, at a
fixed gain:

| Method | What it runs |
| --- | --- |
| `full` | `rf_set_channel()`: the Wi-Fi driver, the frequency, the whole receive lock |
| `direct` | `phy_set_freq()` alone, from the Wi-Fi centre of R4 |
| `force` | `direct`, then the gain forced |
| `agc+force` | `force` with `phy_disable_agc()` |
| `rfagc+force` | `force` with `phy_rfagc_disable()` |
| `direct+gain` | `force` with both |
| `direct+restore` | `direct`, then the whole receive lock replayed (`rf_set_filter()`) |

The `HOPBENCH step` lines come first: each call of `direct+gain` on its own, then what
copying a window and computing its power cost the CPU.

Reading a line, `min/median/max` over the sweeps:

- `hop_us`: how long the call takes.
- `settle_us`: how long the IQ power then takes to stay within about 3 dB of its final
  level, a lone odd window ignored (a live VTX moves more than 1 dB from one window to the
  next). **Read it only on the VTX's channel, VTX on**: noise alone says nothing about the
  lock. It is an upper bound, about 0.2 ms late (a window is the newest completed 102 µs
  DMA descriptor). `settled=` counts the hops that held still.
- `power_db`: the settled in-band power. Against the VTX-off run it gives the noise floor,
  the contrast on the VTX's channel and the bleed on its neighbours. `direct` far below
  `full` on a channel means a bare frequency step is not enough there.
- `late_db`: the power 20 ms later. Far from `power_db`, something still moves after the
  trace (gain, DC), and `settle_us` is too optimistic.

The `strongest=` line sums, on the strongest channel (the VTX's, VTX on), the median hop,
its settling, one power window and its CPU cost, and gives `per_channel_hz`, the refresh a
scan of the 8 channels would get. `contrast_db` is that channel against the median of the
others. VTX off, that line means nothing. The `trace` line under it is that channel's power
after the last hop, `microseconds:dB`, to check `settle_us` by eye.

What decides: the issue's option B (one C5 scanning) needs about 50 Hz per channel or more,
with `contrast_db` close to `full`'s and no clipping. If only the slow methods read
correctly, or the refresh stays near 20 Hz, a scan is not worth it: keep one C5 per channel
(options A and C).

Board result (one C5, one static VTX on R6, 2026-11-05):

| Method | Hop | Contrast | Refresh per channel |
| --- | --- | --- | --- |
| `full` | 7.7 ms | 24.9 dB | ~15 Hz |
| `direct` | 0.9 ms | 6.6 dB (the forced gain is lost) | unusable |
| `force` | 0.89 ms | 25.1 dB | **~88 Hz** |
| `agc+force` | 0.94 ms | 22.9 dB | ~85 Hz |
| `rfagc+force` | 4.3 ms | 23.1 dB | ~26 Hz |
| `direct+gain` | 4.3 ms | 20.5 dB | ~26 Hz |
| `direct+restore` | 6.2 ms | 25.9 dB | ~19 Hz |

`phy_set_freq()` drops the forced gain, and forcing it again is enough: `force` reads every
channel like `full`, R1 to R8, from a single Wi-Fi centre. `phy_rfagc_disable()` alone costs
3.4 ms and brings nothing; `phy_disable_agc()` costs 1 µs. A window costs about 0.3 ms of CPU
(the refresh figures above count it; the `per_channel_hz` of that run did not). Still to
prove: real passes, a moving VTX, sensitivity at range with this fixed gain, and hours of
continuous hopping.

## 5. Scan recording: do real passes stand out?

The detector scans by itself ([detector.md](detector.md)); this records the raw power of every
channel, to see whether real passes stand out and to tune it. It is the tail of
`components/checkpoint/hop_bench.c`, on the console only: every bench takes the receiver from
the detector, so there is no detection while it runs, and the detector starts again after it.

The gain is 24 by default, the lowest it takes: at the hop bench's 52 a close VTX sits on the
4-bit IQ's rail and a pass only rises 3-4 dB. `K` then `g` or `G` moves it by 4, from 24 to 78,
not saved. The aim: the VTX near the noise at rest, and around +15 dB at
its closest, below the rail (about +20 dB). The channels are read R1 R5 R2 R6 R3 R7 R4 R8: what follows a
strong channel in time is then never its neighbour in frequency, so a residue of the previous
channel lands on another channel than a real bleed.

1. Log the console to a file (`tio -L --log-file run.log <port>`, or `tools/flash.py <port>
   --monitor-only --log run.log`). Set a VTX on a known channel, with the power low enough
   that it does not saturate up close.
2. `K` then `S` starts it: `SCANBENCH start`.
3. Move the VTX past the antenna a few times, a second or so each, a few seconds apart.
4. `K` then `S` again stops it, **within 60 s of the first pass**: only the last 60 s are
   kept, or less on a tight heap (`SCANBENCH start` tells). The `SCAN,` lines print for a few
   seconds, up to `SCANBENCH done`.
5. Reset the board, then read the recording:

```sh
python3 tools/scan_analyze.py run.log
```

It prints the real refresh, each channel's levels, and the passes: a channel rising
8 dB above its own resting level (`--margin`), which rules out a steady signal, timed at the
centroid of the peak. A pass seen on several channels at once goes to the strongest, and the
others are listed as `also on`.

What decides: every pass found, on the right channel, none invented, and neighbours well
below it. For endurance, leave it running (one hour at most): a `SCANBENCH live` line every
10 s gives the refresh, `no_data` (windows that could not be read) and each channel's peak,
then do a few passes and stop it. The refresh must not sag and passes must still come out.

### Residue after a hop

First board recordings: whatever channel is read right after a strong one shows it about 8 dB
down. It followed the reading order, R7 after R6, then R3 once the order crossed the band, so
it is not a bleed between neighbours, which stay clean (R5 43 dB under R6). `K` then `z` times
it, at the scan's gain, with a VTX close enough to read well above the noise: hops from the
strongest channel to the quietest, waits a delay from 0 to 5 ms, then prints the IQ power of the
four quarters of one window (oldest first, 25 µs each) as `after_db`, and the same hop from
another quiet channel as `control_db`. The first delay where both agree is how long a scan
must wait, and the quarters show which samples to skip. Reset the board afterwards.

Board result (2026-11-06, R4 at +10 dB, gain 40): the residue starts about 4 dB under the strong
channel, falls about 20 dB in the first 200 µs, then lingers near -10 dB; from 1 ms on neither
trace shows anything. Even a hop from a quiet channel leaves a transient over the first 100 µs
and blips near 500 and 800 µs, so it is not only the previous signal. The scan now waits 1 ms
after each hop, which takes it from about 88 Hz to about 57 Hz per channel.

Result with that wait (2026-11-06, onboard antenna, one drone on R4, hand passes): at gain 24,
4 passes out of 4 on R4, at +13 to +16 dB over its rest, with `--margin` 8 or 10 (6 adds four
weak bumps of +7 to +8 dB), and nothing on the other channels. R8, read right after R4, no
longer follows it in any of the nine recordings. At gain 40 the same antenna with the drone
close fills the other channels with bursts over +5 dB, which the analyzer took for passes on
R8. Still to prove: real flight speed, several drones at once, hours of running.

### For later: hardware attenuation, for a more precise pass point

Not needed yet: try the software gain first, it is free and set per gate. Come back to this
when a gate needs to know more precisely where and when a drone passed:

- **A directive or shielded antenna aimed at the gate** is the real gain: it narrows the zone
  the C5 sees, so the pass peak gets sharper and the timing better. `hardware/antenna/` holds
  a first printed design, not simulated or built yet.
- **A fixed attenuator** (20 dB, on an IPEX to SMA lead, rated to 6 GHz: many stop lower) only
  shifts the levels down: useful if the RF stage saturates before the gain setting can help.

The IPEX connector is off by default. The C5-Zero's antenna switch is GPIO 26, high for the
IPEX connector, low for the onboard antenna, and the firmware leaves it alone: with the VTX on
R6, unplugging the IPEX antenna moved `power_db` by only 1-2 dB in `K` `s` on the onboard
antenna, and by over 30 dB after `K` `i` (2026-11-05: +13 dB to the metric's floor). `K` then
`i` switches it at run time (C5-Zero only, not saved, back to onboard after a reset;
`CHECKPOINT antenna=` shows which) and `CONFIG_CHECKPOINT_ANTENNA_IPEX=y` boots on the IPEX
connector, set before the radio starts so the calibration sees that antenna. Recalibrate (`B`,
VTX off) after changing antenna. Too much attenuation misses a drone passing high or
far from the antenna, and every pilot must fly the same power (25 mW and 600 mW are 14 dB apart).
