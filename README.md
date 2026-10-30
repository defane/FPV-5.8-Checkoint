# FPV 5.8 Checkpoint

A two-ESP **"a drone just passed"** platform: it detects a drone pass on the
tuned 5.8 GHz FPV channel and plays a coin sound, with a phone-friendly web UI
to tune it and watch events.

> **Derivative.** This project adapts the receive engine of
> [C5VRX](https://github.com/Twotoz/C5VRX) — **C5VRX-5, by Twotoz and the C5VRX
> contributors** (forked at upstream commit `6bc9e4b`). The upstream RF chain
> (MODEM_DIAG → BitScrambler → PARLIO → AGC and its control/calibration) is
> reused **unchanged** as the `components/c5vrx_rx` component; this repository
> adds the pass detector, the coin sound, and the C5↔C3 platform around it.
> Visit the [official C5VRX website](https://twotoz.github.io/C5VRX/) for the
> original project; its Discord invite is available there. Licensed
> **GPL-3.0-only** (see `LICENSE`); this is **not** an official C5VRX release
> and is not endorsed by its authors.

## What it is

- **ESP32-C5** (`firmware/c5`, Waveshare ESP32-C5-Zero) — runs the full C5VRX
  receive chain, detects a pass from the tuned channel's C/N, and plays a coin
  sound on a MAX98357A I2S amplifier. Sound is the only local output: the video
  DAC is off (`CONFIG_C5VRX_VIDEO_OUTPUT=n`), freeing its pins for the amplifier.
- **ESP32-C3** (`firmware/c3`) — a node bridge: UART to the C5, ESP-NOW uplink to
  the C6 master; if no master is reachable at boot it falls back to a local
  Wi-Fi SoftAP + web (band selection, results).
- **ESP32-C6** (`firmware/c6`) — the optional **race-timing master**: UART to its
  own C5, the centralized speaker, a Wi-Fi AP admin web (pairing, gate order,
  channel assignment, live lap/sector standings), and an ESP-NOW hub that
  aggregates the C3 nodes. Each gate is a C5; the C6 times laps per channel.

## Layout

```
components/
  c5vrx_rx/      upstream C5VRX receive engine, reused unchanged as a component
  checkpoint/    pass detection + coin sound + the C5<->bridge UART link
  pass_detector/ pure-C pass detector (host-tested)
  audio_player/  I2S coin-sound player (the C6 speaker in a fleet)
  link_proto/    pure-C C5<->bridge UART protocol (host-tested; incl. SET_CHANNEL)
  fleet_proto/   pure-C C6<->C3 ESP-NOW protocol (host-tested)
  race_timing/   pure-C lap/sector timing engine (host-tested)
  espnow_link/   ESP-NOW transport carrying fleet_proto (C6 + C3)
firmware/c5/     ESP32-C5 application (RF + detection; sound local or report-only)
firmware/c3/     ESP32-C3 node bridge (UART + ESP-NOW relay, degraded Wi-Fi web)
firmware/c6/     ESP32-C6 master (UART + ESP-NOW hub + speaker + admin web + timing)
docs/            CHECKPOINT.md (feature notes + bench), wiring.svg
```

See **[docs/CHECKPOINT.md](docs/CHECKPOINT.md)** for the feature notes, the fleet
and the bench procedure, and **[docs/wiring.svg](docs/wiring.svg)** for the
single-set wiring (C3, C5 and the amplifier).

## Build

```sh
. ~/esp/esp-idf-v6.0.2/export.sh
( cd firmware/c5 && idf.py build )     # ESP32-C5, must stay at 0 warnings
( cd firmware/c3 && idf.py build )     # ESP32-C3 node bridge
( cd firmware/c6 && idf.py build )     # ESP32-C6 master

# Host tests
cd test
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include \
   test_pass_detector.c ../components/pass_detector/pass_detector.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/audio_player/include \
   test_audio_resample.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/link_proto/include \
   test_link_proto.c ../components/link_proto/link_proto.c -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/fleet_proto/include -I../components/link_proto/include \
   test_fleet_proto.c ../components/fleet_proto/fleet_proto.c ../components/link_proto/link_proto.c -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/race_timing/include \
   test_race_timing.c ../components/race_timing/race_timing.c -o /tmp/t && /tmp/t
```

## Wiring (summary)

C5↔C3 UART link (crossed, common ground): C5 **GPIO6 TX → C3 GPIO4 RX**, C5
**GPIO15 RX ← C3 GPIO5 TX**. Amplifier on the C5: **BCLK 9, LRC 8, DIN 7, SD 10**,
`VIN` 5 V, `OUT±` to a 4 Ω speaker. Full diagram and pin ownership:
[docs/wiring.svg](docs/wiring.svg) and [CLAUDE.md](CLAUDE.md).

## Safety

> [!WARNING]
> This is experimental and built on an experimental FPV receiver. Bugs, signal
> loss or automatic receive-chain changes can cause sudden loss of video or
> sync even after an apparently successful test. Do not rely on it as your only
> means of maintaining safe control of an aircraft.

## Attribution

Receive engine: **C5VRX** by Twotoz and the C5VRX contributors —
<https://github.com/Twotoz/C5VRX> · <https://twotoz.github.io/C5VRX/> (Discord
invite on the website). Reused material keeps its original copyright, license
and credits; see `AGENTS.md` for the reuse policy and `assets/BRANDING.md` for
branding terms.
