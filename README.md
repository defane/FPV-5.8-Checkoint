# FPV 5.8 Checkpoint

A **"a drone just passed"** platform: it detects a drone pass on the tuned
5.8 GHz FPV channel and plays a coin sound, with a phone-friendly web UI to tune
it and watch events. It runs as a two-ESP checkpoint (C5 + C3) and scales to a
C6-mastered, multi-gate race-timing fleet.

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

- **ESP32-C5** (`firmware/detector`, Waveshare ESP32-C5-Zero) — runs the full C5VRX
  receive chain, detects a pass from the tuned channel's C/N, and plays a coin
  sound on a MAX98357A I2S amplifier. Sound is the only local output: the video
  DAC is off (`CONFIG_C5VRX_VIDEO_OUTPUT=n`), freeing its pins for the amplifier.
- **ESP32-C3** (`firmware/node`) — a node bridge: UART to the C5, ESP-NOW uplink to
  the C6 master; if no master is reachable at boot it falls back to a local
  Wi-Fi SoftAP + web (band selection, results).
- **ESP32-C6** (`firmware/master`) — the optional **race-timing master**: UART to its
  own C5, the centralized speaker, a Wi-Fi AP web, and an ESP-NOW hub that
  aggregates the C3 nodes. Each gate is a C5; the C6 times laps per channel.

The web has **two roles**: **spectators** watch the live standings and status
read-only (no login), while **admin** actions — pairing, gate order, channel
assignment, reset, and (on a degraded C3) band selection — require a login
(`CONFIG_*_ADMIN_USER` / `_PASSWORD`, sent as HTTP Basic and checked server-side).

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
  status_led/    single-WS2812 status indicator (pure-C core host-tested)
firmware/detector/  ESP32-C5 application (RF + detection; sound local or report-only)
firmware/node/      ESP32-C3 node bridge (UART + ESP-NOW relay, degraded Wi-Fi web)
firmware/master/    ESP32-C6 master (UART + ESP-NOW hub + speaker + admin web + timing)
docs/            CHECKPOINT.md (feature notes + bench), wiring.svg
```

See **[docs/CHECKPOINT.md](docs/CHECKPOINT.md)** for the feature notes, the fleet
and the bench procedure, and **[docs/wiring.svg](docs/wiring.svg)** for the
single-set wiring (C3, C5 and the amplifier).

## Build

The root `Makefile` is the quick path: `make firmware` builds all four images,
`make flash [FW=<name>] [PORT=<port>]` flashes one (interactive firmware/port
menu if an arg is omitted), `make monitor`, `make clean`. flash/monitor fall
back to esptool / a serial terminal on the already-built image when ESP-IDF is
not installed, so you can build in one place and flash in another. The raw
commands:

```sh
. ~/esp/esp-idf-v6.0.2/export.sh
( cd firmware/detector && idf.py build )  # esp32c5 → build/detector-c5.bin (0 warnings)
( cd firmware/node && idf.py build )      # esp32c3 → build/node-c3.bin
( cd firmware/master && idf.py build )    # esp32c6 → build/master-c6.bin
# temporary "C5 as master" (run the master firmware on a spare C5 until the C6 arrives) → build-c5/master-c5.bin:
( cd firmware/master && idf.py -DIDF_TARGET=esp32c5 -DSDKCONFIG=sdkconfig.c5 \
    -DSDKCONFIG_DEFAULTS=sdkconfig.c5.defaults -B build-c5 build )

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
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/status_led/include \
   test_status_led.c ../components/status_led/status_led.c -o /tmp/t && /tmp/t
```

## Architecture

- **Global schema** (elements + interactions): [docs/fleet.svg](docs/fleet.svg)
- **Pin-level wiring, per set type:**
  - master **C6 + C5**: [docs/wiring-master.svg](docs/wiring-master.svg)
  - node **C3 + C5**: [docs/wiring-node.svg](docs/wiring-node.svg)
  - temporary **C5 + C5** (a spare C5 runs the master firmware until the C6 arrives): [docs/wiring-c5c5.svg](docs/wiring-c5c5.svg)
  - standalone **C5** (detects + sounds locally, optional C3 web): [docs/wiring.svg](docs/wiring.svg)

## Pinout

Each UART link is crossed (one side's TX to the other's RX) with a common
ground. Pins are Kconfig defaults (`menuconfig`); the C6 defaults should be
checked against your board's strapping pins. USB-Serial-JTAG (flashing/console)
is reserved on every board.

**ESP32-C5** (`firmware/detector`, detector — one per gate)

| Function | GPIO | Notes |
| --- | --- | --- |
| Link UART TX → bridge RX | 6 | to a C3 (node) or the C6 (master) |
| Link UART RX ← bridge TX | 8 | C5-Zero header pin (GPIO 15 is not broken out); the amp LRC, free on a fleet gate |
| Amplifier BCLK / LRC / DIN / SD | 9 / 8 / 7 / 10 | only when the C5 sounds locally (standalone); off in a fleet |
| forbidden | 0-5, 25, 27 (IQ) · 13,14 (USB) · 16-22 (flash) · 26 (antenna) | receiver-owned |

**ESP32-C3** (`firmware/node`, node bridge)

| Function | GPIO | Notes |
| --- | --- | --- |
| UART TX → C5 RX (8) | 5 | |
| UART RX ← C5 TX (6) | 4 | |
| Wi-Fi / ESP-NOW | — | no GPIO; ESP-NOW to the C6 on the shared AP channel |

**ESP32-C6** (`firmware/master`, master)

| Function | GPIO | Notes |
| --- | --- | --- |
| UART TX → local C5 RX (8) | 4 | |
| UART RX ← local C5 TX (6) | 5 | |
| Amplifier BCLK / LRC / DIN / SD | 18 / 19 / 20 / 21 | the centralized speaker; `VIN` 5 V, `OUT±` to a 4 Ω speaker |
| Wi-Fi AP + ESP-NOW | — | admin web + fleet hub; same channel as the nodes |

The C6 AP, the C3 APs and ESP-NOW must share the **same Wi-Fi channel**
(`CONFIG_C6_AP_CHANNEL` / `CONFIG_C3_AP_CHANNEL`).

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
