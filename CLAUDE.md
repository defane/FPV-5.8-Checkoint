# FPV 5.8 Checkpoint

A "drone just passed" platform built on C5VRX-5 (upstream:
https://github.com/Twotoz/C5VRX, remote `upstream`): a two-ESP checkpoint
(C5 + C3) that scales to a C6-mastered race-timing fleet (see Fleet below).

- **ESP32-C5** (Waveshare ESP32-C5-Zero, `firmware/detector`): the full C5VRX RF
  receive chain → pass detection on the tuned channel's C/N → a coin sound.
  **The only local output is the sound**: no video
  (`CONFIG_C5VRX_VIDEO_OUTPUT=n` in `firmware/detector/sdkconfig.defaults`; the
  transport runs unchanged but `s_dac_gpio` routes it to no pin).
- **ESP32-C3** (`firmware/node`): the bridge. UART to the C5; when a C6 master is
  reachable it relays over ESP-NOW (headless), otherwise it falls back to a
  local Wi-Fi SoftAP + web UI that tunes the detector and shows its live C/N,
  stats and passes.

It also scales to a **race-timing fleet** (`docs/CHECKPOINT.md`): an **ESP32-C6
master** (`firmware/master`, UART to its own C5 + the speaker + a Wi-Fi AP admin
web) aggregates passes from remote **C3 nodes** over **ESP-NOW**, each node a
gate wired to a C5. The C6 orders gates, pairs nodes, assigns channels (retune
via `SET_CHANNEL`), times laps/sectors per channel, and plays the central sound.
A C3 that cannot reach the C6 at boot falls back to a degraded local AP+web
(band selection + results).

C5VRX is consumed as a **component**, never edited in place to add a feature:
`components/c5vrx_rx` is the upstream receive engine (RF/transport/gain/agc/
control/calibration/console), re-packaged unchanged. Read `AGENTS.md` first:
it is upstream's agent policy (attribution, realtime invariants, sources) and
applies here too. Full feature notes and the bench procedure:
`docs/CHECKPOINT.md`.

## Layout

```
components/
  c5vrx_rx/      upstream C5VRX receive engine (consumable; INCLUDE_DIRS ".")
  checkpoint/    our glue: detector + audio + NVS + the C5<->C3 UART link
  pass_detector/ pure-C pass detector (host-tested)
  audio_player/  I2S coin-sound player (the C6 speaker in a fleet)
  link_proto/    pure-C C5<->bridge UART protocol (host-tested; incl. SET_CHANNEL)
  fleet_proto/   pure-C C6<->C3 ESP-NOW protocol (host-tested)
  race_timing/   pure-C lap/sector timing engine (host-tested)
  espnow_link/   ESP-NOW transport (C6 + C3) carrying fleet_proto
  status_led/    single-WS2812 status indicator (pure-C core host-tested)
firmware/detector/  ESP32-C5 app (RF + detection; sound local, or report-only in a fleet)
firmware/node/      ESP32-C3 node bridge (UART + ESP-NOW relay, degraded Wi-Fi web)
firmware/master/    ESP32-C6 master (UART + ESP-NOW hub + speaker + admin web + timing)
```

The console/boot calls into checkpoint are kept without a circular dependency
by weak no-op stubs (`components/c5vrx_rx/checkpoint_weak.c`); the `checkpoint`
component supplies the strong overrides. So `c5vrx_rx` also builds standalone
as plain C5VRX.

## Priorities

- The receive chain comes first: MODEM_DIAG, BitScrambler, PARLIO, AGC
  (`components/c5vrx_rx/rf.c`, `transport.c`, `receiver.c`, `gain.c`,
  `agc.c`, `*.bsasm`) are **not modified** without the operator's approval;
  the engine was moved into the component by pure file renames. The
  checkpoint code only reads receiver state.
- Every checkpoint task stays at priority 1, below the receiver tasks
  (gain_fast 4, control 3, gain_obs 3). The C5<->C3 UART task is priority 1 too.
- RAM is tight: ~2 KB free at the boot peak without PSRAM. Checkpoint
  state is heap-allocated after the splash (`checkpoint_start()` in
  `firmware/detector/main/main.c`); do not add static `.bss` buffers.
- Conventional Commits, one commit per feature.

## Wiring

MAX98357A I2S amplifier, 5 V supply, 4 Ω speaker, on the pins freed by
the video output (Kconfig defaults):

| Amplifier | GPIO |
| --- | --- |
| BCLK | 9 |
| LRC (WS) | 8 |
| DIN | 7 (strapping pin: only a high-impedance input on it, never SD) |
| SD (HIGH = on, LOW between sounds) | 10 |

C5 ↔ C3 UART link (`CONFIG_CHECKPOINT_LINK_*`, cross-wired to the C3's TX/RX):

| C5 link | GPIO |
| --- | --- |
| UART TX (→ C3 RX) | 6 |
| UART RX (← C3 TX) | 8 |

The first amplifier wiring (GPIO3 BCLK, 4 LRC, 5 DIN, 1 SD) sat on the IQ
loopback (`RF_IQ_GPIO_NUMS = {1, 0, 25, 2, 27, 5, 3, 4}`,
`components/c5vrx_rx/rf.h`, driven at 40 MS/s and needed for the C/N even
without video) and was abandoned. Audio is off until `coin.pcm` exists
(`CONFIG_AUDIO_PLAYER_ENABLE=n`); pins are checked at boot by
`components/checkpoint/checkpoint.c:pin_owner()`.

## GPIO map

| GPIO | Owner | Notes |
| --- | --- | --- |
| 0, 1, 2, 3, 4, 5, 25, 27 | IQ loopback (MODEM_DIAG → PARLIO RX) | forbidden; GPIO-matrix loopback, no wiring; 27 is also the C5-Zero onboard WS2812, so that LED is unusable here |
| 8, 9, 11, 12, 23, 24 | 6-bit video DAC (PARLIO TX) | **free with video off** (default here) |
| 7, 10 | reserved 7th/8th DAC branches | free with video off; 7 is a strapping pin (never SD) |
| 13, 14 | USB-Serial-JTAG (console, flashing) | forbidden |
| 16–22 | SPI flash | forbidden |
| 26 | antenna switch (C5-Zero) | forbidden |
| 28 | BOOT button | input, strapping |
| 6, 8 | C5 ↔ C3 UART link | TX 6, RX 8 (C5-Zero header pins; 8 is the amp LRC, free on a fleet gate / video off; GPIO 15 is not broken out) |

GDMA: 3 AHB pairs. C5VRX uses 1 RX (PARLIO RX) and 1 TX (PARLIO TX; the TX
BitScrambler is inline, no channel of its own). I2S gets a free TX channel,
created per burst of sounds because `transport.c:quiet_interrupts()` turns
off all three channels' interrupts on every transport restart.

## Detector (`components/pass_detector`, pure C)

Input: the tuned channel's C/N in dB (`receiver_obs_cn_x10()`), every 50 ms.
Channel slot = `rf_channel_index() % 8` (R1..R8 → 0..7). States
`IDLE → RISING → PEAK_PASSED → COOLDOWN`; re-arm needs the refractory period
elapsed AND the level under `active_db - hysteresis_db`. The glue is
`components/checkpoint`; the parameters are editable from the console
(`K`+key, `components/c5vrx_rx/console.c`) and the C3 web UI (pushed over the
link as `CONFIG_SET`, echoed as `CONFIG_STATE`), saved in NVS `checkpoint`
(`checkpoint_prefs_t` — bump `PREFS_VERSION` on a layout change). Parameter
table (defaults + console keys): **`docs/CHECKPOINT.md` → "Paramètres"**.

## Status LED

A board's onboard WS2812 shows state via `components/status_led` (pure-C
state→colour core, host-tested; RMT WS2812 driver; boot red→green→blue
self-test; priority-1 task; ~15%). Onboard GPIO: node **10**, master **8**
(C5-as-master **27**). **The detector has no status LED** — the C5-Zero WS2812 is
on **GPIO27**, a receiver IQ-loopback pin, so it is disabled
(`CONFIG_STATUS_LED_ENABLE=n`); a C3 SuperMini has no WS2812 either. Full scheme
(colours per state and role): **`docs/CHECKPOINT.md` → "LED de statut"**.

## Fleet (C6 master + C3 nodes)

Race timing across ordered gates (each gate = a C5). The **C6** (`firmware/master`,
esp32c6) is UART-wired to its own C5 (gate 0) and aggregates remote gates over
**ESP-NOW**; a remote gate is a C5 UART-wired to a **C3** (`firmware/node`). Global
schema: `docs/fleet.svg`.

- **Protocols**: UART `components/link_proto` (C5↔bridge: passes, config, and
  `LP_MSG_SET_CHANNEL` which makes the C5 call `rf_set_channel()` — the only
  place checkpoint commands a retune rather than reading); ESP-NOW
  `components/fleet_proto` (pairing, time beacons, `CONFIG`, `GATE_PASS`,
  `NODE_STATUS`) over `components/espnow_link`; `components/race_timing`
  (lap/sector per channel, host-tested).
- **Pinout** (Kconfig defaults; crossed UART, common ground):

  | Board | UART TX / RX | Amplifier (BCLK/LRC/DIN/SD) |
  | --- | --- | --- |
  | C5 (gate) | 6 / 8 → bridge | 9/8/7/10 (local sound only; off in a fleet) |
  | C3 (node) | 5 / 4 → C5 | — |
  | C6 (master) | 4 / 5 → local C5 | 18/19/20/21 → 4 Ω speaker |

  C6 amp/UART pins are placeholders — check against the C6 board's strapping
  pins. The C6 AP, the C3 APs and ESP-NOW share one Wi-Fi channel
  (`CONFIG_C6_AP_CHANNEL` / `CONFIG_C3_AP_CHANNEL`).
- **Sound** is centralized on the C6 (remote C5s report only).
- **Web roles** (C6 admin web and the C3 degraded web): a **spectator** reads
  standings/status with no login; **admin** actions need a login
  (`CONFIG_*_ADMIN_USER`/`_PASSWORD`, HTTP Basic, checked server-side on every
  mutation endpoint; empty password disables auth).
- **Degraded C3**: if the C6 is not reached within `CONFIG_C3_FLEET_TIMEOUT_MS`
  at boot, the C3 serves its own AP + web (band selection → `SET_CHANNEL`,
  results) instead of relaying.
- **Temporary C5-as-master** (until a C6 is available): run `firmware/master` on a
  spare C5 — `idf.py -DIDF_TARGET=esp32c5 -DSDKCONFIG=sdkconfig.c5
  -DSDKCONFIG_DEFAULTS=sdkconfig.c5.defaults -B build-c5 build` from
  `firmware/master` (→ `build-c5/master-c5.bin`, vs `master-c6.bin` on the C6).
  The amp moves to the C5 pins 9/8/7/10 (GPIO 16-22 are the C5
  flash); UART stays 4/5. Diagram: `docs/wiring-c5c5.svg`.

## Build and tests

The root `Makefile` wraps the per-firmware commands: `make firmware` builds every
variant, `make flash [FW=<name>] [PORT=<port>]` flashes one (interactive
firmware/port menu when an arg is omitted), plus `make monitor`, `make variants`
(list the `FW=` names) and `make clean`. A `FW=` name is a role (`detector`/
`node`/`master`, its default board) or a `<role>-<board>` variant
(`detector-xiao-c5`, `detector-wave-c5-lcd`, `master-wave-c5`); each variant is an
overlay on the role's `sdkconfig.defaults` and builds in its own dir. flash/monitor fall back to esptool / a serial terminal on the
already-built image when ESP-IDF is not installed (build it elsewhere, flash
here). The raw commands:

```sh
. ~/esp/esp-idf-v6.0.2/export.sh
( cd firmware/detector && idf.py build )  # esp32c5 → detector-c5.bin (0 warnings)
( cd firmware/node && idf.py build )      # esp32c3 → node-c3.bin
( cd firmware/master && idf.py build )    # esp32c6 → master-c6.bin
cd test     # host tests (engine headers now at ../components/c5vrx_rx)
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include \
  test_pass_detector.c ../components/pass_detector/pass_detector.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/audio_player/include test_audio_resample.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/link_proto/include \
  test_link_proto.c ../components/link_proto/link_proto.c -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/fleet_proto/include -I../components/link_proto/include \
  test_fleet_proto.c ../components/fleet_proto/fleet_proto.c ../components/link_proto/link_proto.c -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/race_timing/include \
  test_race_timing.c ../components/race_timing/race_timing.c -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/status_led/include \
  test_status_led.c ../components/status_led/status_led.c -o /tmp/t && /tmp/t
```

CI (`.github/workflows/c5vrx5.yml`) builds all three firmwares and runs the host
tests (the upstream engine tests compile against `components/c5vrx_rx`). Sound:
`python3 tools/coin_sound.py` (needs `assets/coin_source.mp3`, not versioned).
`test/test_manual_calibration_runtime.py` already fails upstream with recent
host gcc (`-Wformat-truncation` in `dco_slots.h`).
