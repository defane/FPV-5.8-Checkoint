# FPV 5.8 Checkpoint

A "drone just passed" platform built on C5VRX-5 (upstream:
https://github.com/Twotoz/C5VRX, remote `upstream`): a two-ESP checkpoint
(C5 + C3) that scales to a C6-mastered race-timing fleet (see Fleet below).

- **ESP32-C5** (Waveshare ESP32-C5-Zero, `firmware/detector`): the full C5VRX RF
  receive chain → pass detection on the tuned channel's C/N → a pass report over
  the UART link. **No local output at all**: no video
  (`CONFIG_C5VRX_VIDEO_OUTPUT=n` in `firmware/detector/sdkconfig.defaults`; the
  transport runs unchanged but `s_dac_gpio` routes it to no pin), **no amplifier**
  (the sound is the C6 master's) and no status LED.
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
control/calibration/console, commit `6bc9e4b`), **pruned for the video-off
detector**: the menu/OSD/splash, field experiments, flight log and bench labs
are removed and replaced by inert stubs (`menu_stub.c`, `pruned_stubs.c`), the
console key table is reduced; `transport.c` carries the fork's 7-line
`VIDEO_OUTPUT=n` pin routing; every other engine file is byte-identical to
upstream (every change is listed in `AGENTS.md` "Repository sources"). Read `AGENTS.md` first:
it is upstream's agent policy (attribution, realtime invariants, sources) and
applies here too. Full feature notes and the bench procedure:
`docs/CHECKPOINT.md`.

## Layout

```
components/
  c5vrx_rx/      upstream C5VRX receive engine, pruned (consumable; INCLUDE_DIRS ".")
  checkpoint/    our glue: detector + NVS + the C5<->C3 UART link (no audio, no LED)
  pass_detector/ pure-C pass detector (host-tested)
  audio_player/  I2S coin-sound player (the C6 speaker in a fleet)
  link_proto/    pure-C C5<->bridge UART protocol (host-tested; incl. SET_CHANNEL)
  fleet_proto/   pure-C C6<->C3 ESP-NOW protocol (host-tested)
  race_timing/   pure-C lap/sector timing engine (host-tested)
  espnow_link/   ESP-NOW transport (C6 + C3) carrying fleet_proto
  status_led/    single-WS2812 status indicator (pure-C core host-tested)
  web_auth/      signed admin requests for the C3/C6 web (pure-C core + auth.js host-tested)
firmware/detector/  ESP32-C5 app (RF + detection, report-only)
firmware/node/      ESP32-C3 node bridge (UART + ESP-NOW relay, degraded Wi-Fi web)
firmware/master/    ESP32-C6 master (UART + ESP-NOW hub + speaker + admin web + timing)
```

The console/boot calls into checkpoint are kept without a circular dependency
by weak no-op stubs (`components/c5vrx_rx/checkpoint_weak.c`); the `checkpoint`
component supplies the strong overrides. `c5vrx_rx` still builds without it,
as a receiver with no on-screen menu, OSD or boot logo (with
`CONFIG_C5VRX_VIDEO_OUTPUT=y` the live video still reaches the DAC pins; the
detector sets it to `n`).

## Priorities

- The receive chain comes first: MODEM_DIAG, BitScrambler, PARLIO, AGC
  (`components/c5vrx_rx/rf.c`, `transport.c`, `receiver.c`, `gain.c`,
  `agc.c`, `*.bsasm`) are **not modified** without the operator's approval;
  the engine was moved into the component by pure file renames, then pruned
  without touching them (byte-identical to upstream, except `transport.c`'s
  7-line `VIDEO_OUTPUT=n` pin routing). The checkpoint code only reads receiver
  state, except what a bridge asks for over the link: a retune (`SET_CHANNEL`,
  saved with `settings_save()`) and a calibration (`filter_request()` /
  `filter_request_full()`, the console's F/B) or reboot (`COMMAND`). Production
  has no console, so every operator need goes through the link.
- The splash window is not cosmetic: `menu_stub.c` keeps its flag (BOOT_PHASE
  runs only under it) and the menu's TX ownership (calibrations and profile
  switches rely on it). Never turn `menu_splash_active()` or
  `menu_status_active()` into constants.
- Every checkpoint task stays at priority 1, below the receiver tasks
  (gain_fast 4, control 3, gain_obs 3). The C5<->C3 UART task is priority 1 too.
- RAM: the prune freed ~107 KB of static SRAM (HP SRAM 290 → 181 KB used, mostly
  the menu's 100 KB raster). Before it, only ~2 KB was free at the boot peak
  without PSRAM; re-measure with `T` (`heap_min`) on a board. Checkpoint state is
  still heap-allocated after the splash (`checkpoint_start()` in
  `firmware/detector/main/main.c`); do not add static `.bss` buffers.
- Conventional Commits, one commit per feature.

## Wiring

A detector is wired to its power and the crossed C5↔bridge UART link
(`CONFIG_CHECKPOINT_LINK_*`) only: **TX 6, RX 8**. **No amplifier on a detector**
(the MAX98357A sits on the C6 master); `sdkconfig.defaults` forces
`CONFIG_AUDIO_PLAYER_ENABLE=n` and `CONFIG_STATUS_LED_ENABLE=n`, because the
shared `components/` dir builds those components into every firmware. Full
per-board pinout: README "Pinout" + `docs/wiring-*.svg`.

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
| 6, 8 | C5 ↔ C3 UART link | TX 6, RX 8 (C5-Zero header pins, free with video off; GPIO 15 is not broken out) |

GDMA: 3 AHB pairs. C5VRX uses 1 RX (PARLIO RX) and 1 TX (PARLIO TX; the TX
BitScrambler is inline, no channel of its own). The detector uses no other one
(no I2S). `transport.c:quiet_interrupts()` turns off all three channels'
interrupts on every transport restart, so never add a long-lived GDMA user here.

## Detector (`components/pass_detector`, pure C)

Input: the tuned channel's C/N in dB (`receiver_obs_cn_x10()`), every 50 ms.
Channel slot = `rf_channel_index() % 8` (R1..R8 → 0..7). States
`IDLE → RISING → PEAK_PASSED → COOLDOWN`; re-arm needs the refractory period
elapsed AND the level under `active_db - hysteresis_db`. The glue is
`components/checkpoint`; the parameters are editable from the console
(`K`+key, `components/c5vrx_rx/console.c`) and the C3 web UI (pushed over the
link as `CONFIG_SET`, echoed as `CONFIG_STATE`), saved in NVS `checkpoint`
(`checkpoint_prefs_t` — bump `PREFS_VERSION` on a layout change). Parameter
table (defaults + console keys): **`docs/detector.md`**.

## Status LED

A board's onboard WS2812 shows state via `components/status_led` (pure-C
state→colour core, host-tested; RMT WS2812 driver; boot red→green→blue
self-test; priority-1 task; ~15%). Onboard GPIO: node **10**, master **8**
(C5-as-master **27**). **The detector has no status LED** — the C5-Zero WS2812 is
on **GPIO27**, a receiver IQ-loopback pin, so the checkpoint glue has no LED code
and `CONFIG_STATUS_LED_ENABLE=n`; a C3 SuperMini has no WS2812 either. Full scheme
(colours per state and role): **`docs/status-led.md`**.

## Fleet (C6 master + C3 nodes)

Race timing across ordered gates (each gate = a C5). The **C6** (`firmware/master`,
esp32c6) is UART-wired to its own C5 (gate 0) and aggregates remote gates over
**ESP-NOW**; a remote gate is a C5 UART-wired to a **C3** (`firmware/node`).
Protocols (both v2; reflash every board together after a change):
`components/link_proto` (C5↔bridge UART: config, passes, `STATS` with the C5's
health, `LP_MSG_SET_CHANNEL` and `LP_MSG_COMMAND` calibrate/reboot),
`components/fleet_proto` over `components/espnow_link` (pairing, beacons,
`GATE_PASS`, `NODE_STATUS` relaying the health, `FP_MSG_COMMAND`),
`components/race_timing`. The C6 admin web (and a degraded C3's page) shows each
gate's health and runs its calibrations/reboot, VTX-guarded:
`docs/fleet.md` "Gate health and remote commands". Sound is centralized on
the C6 (remote C5s report only). A C3 that cannot reach the C6 within
`CONFIG_C3_FLEET_TIMEOUT_MS` falls back to a degraded local AP+web. The C6/C3 APs
and ESP-NOW share one Wi-Fi channel (`CONFIG_C6_AP_CHANNEL`/`CONFIG_C3_AP_CHANNEL`).

Pinout (Kconfig defaults; crossed UART, common ground):

| Board | UART TX / RX | Amplifier (BCLK/LRC/DIN/SD) |
| --- | --- | --- |
| C5 (gate) | 6 / 8 → bridge | — (never on a detector) |
| C3 (node) | 5 / 4 → C5 | — |
| C6 (master) | 4 / 5 → local C5 | 18/19/20/21 → 4 Ω speaker |

C6 amp/UART pins are placeholders — check against the C6 board's strapping pins.
**Temporary C5-as-master**: run the master on a spare C5 (`make flash
FW=master-wave-c5`; amp on 9/8/7/10, UART 4/5). Protocols in depth, pairing, web
roles (spectator vs admin password), degraded mode and the C5-as-master build:
`docs/fleet.md`, `docs/fleet.svg`, `docs/wiring-c5c5.svg`.

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
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/web_auth/include \
  test_web_auth.c ../components/web_auth/web_auth.c -o /tmp/t && /tmp/t
node test_web_auth.mjs   # auth.js (page side of web_auth)
```

CI (`.github/workflows/c5vrx5.yml`) builds all three firmwares and runs the host
tests (the upstream engine tests compile against `components/c5vrx_rx`). Sound:
`python3 tools/coin_sound.py` (needs `assets/coin_source.mp3`, not versioned).
`test/test_manual_calibration_runtime.py` already fails upstream with recent
host gcc (`-Wformat-truncation` in `dco_slots.h`).
