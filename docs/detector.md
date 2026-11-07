# Detector: operation and parameters

The C5 sweeps R1 to R8 all the time and detects a pass on any of them: nobody picks a
channel (`components/checkpoint/checkpoint.c`, `scan_hop.c`; pure C:
`components/pass_detector/scan_detector.c`, `pass_detector.c`).

- **Signal:** the IQ power of one 100 µs window per channel, about 57 times per second
  per channel. The hop is `phy_set_freq()` then the gain forced again (under 1 ms), then
  1 ms for what the previous channel leaves to clear ([bench.md](bench.md)). The channels
  are read R1 R5 R2 R6 R3 R7 R4 R8, so the channel read after a strong one is never its
  neighbour in frequency.
- **Gain:** fixed, 24 by default (onboard antenna, close passes). `K` then `g`/`G` moves it
  by 4, not saved. No AGC during a sweep.
- **Rest:** each channel has its own resting level in dB (the noise differs by up to 13 dB
  across the band). It follows the channel's low values, learns faster for the first 3 s
  of a sweep, stops climbing while the channel is active, and may not climb more than
  6 dB above the other channels' median. So a VTX left on keeps showing, and a drone
  waiting by the gate does not become the channel's rest.
- **Level:** the rise above the rest, in dB, is what goes to the state machine and what
  the web pages show. Under -5 dB the 4-bit IQ only flickers, so a channel has to reach
  that to count, whatever its rest.
- **No sample** while a calibration or a console bench owns the receiver: the sweep lets
  go of it within one sweep and starts over afterwards, rests included. Nothing rings for
  the first 3 s, while the rests settle.
- **Owns the receiver** like `scan()` does: `receiver_pause` + `s_scan_active`, so the
  control loop stops reading windows and steering AFC. The MODEM_DIAG, BitScrambler,
  PARLIO and AGC chain is not modified; the hop calls libphy past `rf.c`
  (operator-approved).
- **State machine** per channel: `IDLE → RISING → PEAK_PASSED → COOLDOWN` (see
  `pass_detector.h`). A hovering drone only rings once: to re-arm, the refractory period
  must have elapsed **and** the level must have dropped back under
  `active_db − hysteresis_db`.
- **One line per pass:**
  `PASS channel=R3 freq=5732 peak_db=15.4 peak_ms=… at_ms=… delay_ms=…`, and sent over the
  UART link as an `EVENT_PASS` (the C6 master plays the sound), timed at the peak.
- **Side by side:** two channels peaking together are two passes. No pass is dropped as
  bleed from another channel: at gain 24 the board shows none, but at gain 40 a close
  drone fills other channels with bursts, which would ring.

## Parameters (console `K`, saved in NVS "checkpoint")

| Parameter | Default | Keys |
| --- | --- | --- |
| `alpha` (exponential smoothing per sweep, weight of a new sample) | 0.25 | `a`/`A` ±0.05 |
| `active_db` (activity threshold, rise above the rest) | 8 dB | `t`/`T` ±1 dB |
| `hysteresis_db` | 3 dB | `h`/`H` ±0.5 dB |
| `drop_db` (fall below the peak that marks a pass) | 3 dB | `d`/`D` ±0.5 dB |
| `min_peak_db` (below it, the drone is too far: no pass) | 10 dB | `m`/`M` ±1 dB |
| `refractory_ms` | 1500 ms | `r`/`R` ±250 ms |
| `stale_ms` | 1000 ms | (fixed) |

The defaults reproduce the board's hand passes: 4 out of 4 on the recording at gain 24, at
+13 to +16 dB, with nothing on the other channels, weak bumps reaching +8 dB. They are not
yet proven on a real flight.

Other keys: `K` then `l` (live rise of R1-R8, 5 lines/s), `g`/`G` (gain), `0` (defaults),
`k` (show), `s` (hop bench: [bench.md](bench.md#4-hop-bench-can-one-c5-scan-raceband)),
`S` (scan recording: [bench.md](bench.md#5-scan-recording-do-real-passes-stand-out)),
`z` (residue trace: same page), `i` (antenna onboard/IPEX, C5-Zero only: GPIO 26),
`q` (close). The `K` window stays open 5 s after each key. At the top
level, `T` prints the `CHECKPOINT` line.

**Setting `active_db`:** put it below the rise of a drone passing the gate, but above the
rise of the same drone elsewhere on the track. Otherwise the channel never re-arms between
laps.
