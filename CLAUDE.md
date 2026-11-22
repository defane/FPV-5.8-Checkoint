# FPV 5.8 Checkpoint

A "drone just passed" platform, forked from C5VRX-5 (upstream:
https://github.com/Twotoz/C5VRX, remote `upstream`): a two-ESP checkpoint
(C5 + C3) that scales to a C6-mastered race-timing fleet (see Fleet below).

- **ESP32-C5** (Waveshare ESP32-C5-Zero, `firmware/detector`): the **sdr_rx** engine
  (`components/sdr_rx`): snapshots of the PHY's I/Q dump at 80 MS/s, one LO for two
  channels (5677/5751/5825/5899 MHz), a 4 × 64-point integer PSD with moment-based I/Q
  correction, ±5 MHz band power per channel, ~500 Hz per channel (widscan58 measured
  ~510 Hz; bench B1 measures ours) → pass detection on any of R1-R8 → a pass report over
  the UART link. **No local output at all**: no video, **no amplifier** (the sound is
  the C6 master's) and no status LED.
- **ESP32-C3** (`firmware/node`): the bridge. UART to the C5; when a C6 master is
  reachable it relays over ESP-NOW (headless), otherwise it falls back to a
  local Wi-Fi SoftAP + web UI that tunes the detector and shows its channels' levels,
  stats and passes.

It also scales to a **race-timing fleet** (`docs/CHECKPOINT.md`): an **ESP32-C6
master** (`firmware/master`, UART to its own C5 + the speaker + a Wi-Fi AP admin
web) aggregates passes from remote **C3 nodes** over **ESP-NOW**, each node a
gate wired to a C5. The C6 orders gates, pairs nodes, times laps/sectors per channel
(every gate sees every channel, nobody picks one), and plays the central sound.
A C3 that cannot reach the C6 at boot falls back to a degraded local AP+web
(results + detector settings).

The engine is ported from widscan58 (CapnBry, `a9fff97`), whose radio layer derives
from esp-sdr (ESPARGOS, `a8eaac1`): the ported files keep their GPL-3.0-or-later
headers, plus a provenance line and their changes. The project was forked from
C5VRX-5 (upstream `6bc9e4b`), whose receive engine was removed on this branch
(issue #2); `main` keeps it until a board bench validates sdr_rx. Read `AGENTS.md`
first: it is upstream's agent policy (attribution, research provenance) and still
governs the C5VRX material this tree keeps (the I/Q dump research, the console
structure, the firmware id and NVS helpers, `tools/flash.py`). Credits: README
"Attribution". Full feature notes: `docs/CHECKPOINT.md`; bench: `docs/bench.md`.

## Layout

```
components/
  sdr_rx/        receive engine (C5 only): dump capture, LO pairs, PSD + IQ correction (pure-C parts host-tested)
  checkpoint/    our glue: the scan task, console, benches, detector + NVS + the C5<->C3 UART link (no audio, no LED)
  pass_detector/ pure-C pass detector and scan detector (host-tested)
  audio_player/  I2S coin-sound player (the C6 speaker in a fleet)
  link_proto/    pure-C C5<->bridge UART protocol (host-tested)
  fleet_proto/   pure-C C6<->C3 ESP-NOW protocol (host-tested)
  race_timing/   pure-C lap/sector timing engine (host-tested)
  espnow_link/   ESP-NOW transport (C6 + C3) carrying fleet_proto
  status_led/    single-WS2812 status indicator (pure-C core host-tested)
  web_auth/      signed admin requests for the C3/C6 web (pure-C core + auth.js host-tested)
firmware/detector/  ESP32-C5 app (RF + detection, report-only)
firmware/node/      ESP32-C3 node bridge (UART + ESP-NOW relay, degraded Wi-Fi web)
firmware/master/    ESP32-C6 master (UART + ESP-NOW hub + speaker + admin web + timing)
hardware/           carrier PCBs, node (C5+C3) and master (C5+C6+amp): generator, KiCad files, Gerbers
hardware/antenna/   5.8 GHz quasi-Yagi: gen_qyagi.py is its source, sim_qyagi.py its openEMS model; not built yet
hardware/enclosure/ 3D-printed node/master box: enclosure.sc (SolidCraft, a configuration per set) is its source, print/ its 3MF parts
                    (edit enclosure.sc in place with solidcraft-cli, then re-export the 3MFs: README "Editing the design")
```

Every firmware builds only `main`'s dependency closure (`set(COMPONENTS main)`): of
`components/`, the detector links `checkpoint`, `sdr_rx`, `pass_detector` and
`link_proto` only. `sdr_rx` is found by the C3/C6 builds but built only for the esp32c5.

## Priorities

- The engine owns the PHY, and only the scan task calls it
  (`components/checkpoint/cp_scan.c`: `sdr_rx_step`, the requests, the benches). The
  console and the link only post requests (the mailbox in `cp_internal.h`); the scan task
  applies them between two hops. Production has no console, so every operator need goes
  through the link (`COMMAND`: F, B, reboot).
- It depends on a private libphy ABI (`phy_param` offsets, wrapped internals; listed in
  `sdr_radio_c5.c`'s header), pinned by SHA-256 to IDF 6.0.2's archive: CMake fails on
  another one (`components/sdr_rx/CMakeLists.txt`).
- Changes to `sdr_radio_c5.c`, `sdr_recal_c5.c`, `hot_iram.lf`, `sram_guard.ld` or the
  `--wrap` list need a board bench (`docs/bench.md` "Engine bench"). Keep them close to
  widscan58 and record each deviation in the file's header.
- Tasks: the scan task is priority 1, blocks one tick every 100 ms in every loop (scan,
  benches, recording: `cp_yield()`) and is watched by the task watchdog (panic: a hung
  gate reboots). The C5<->C3 UART task (`cp_link`) is priority 1 too, console 2,
  usb_reader 10 (the download key). Benches run in the scan task, never in the console.
- RAM: static IRAM, data and BSS must end below `0x40820000` (`sram_guard.ld` fails the
  link otherwise) with an 8 KiB margin: `_iram_end`, `_data_end`, `_bss_end` ≤
  `0x4081E000` (`riscv32-esp-elf-nm`; `_bss_end` is `0x4081b018` today). Buffers go on
  the heap or in the bank's regions (`sdr_bench_scratch()`); checkpoint state is
  heap-allocated (`checkpoint_start()`), so do not add static `.bss` buffers. Nothing but
  the scan task touches `0x40820000`–`0x4083FFFF` (the RF dump bank), never during a
  capture.
- Conventional Commits, one commit per feature.

## Wiring

A detector is wired to its power and the crossed C5↔bridge UART link
(`CONFIG_CHECKPOINT_LINK_*`) only: **TX 6, RX 8**. **No amplifier on a detector**
(the MAX98357A sits on the C6 master). The detector builds `COMPONENTS main`, so
neither `audio_player` nor `status_led` is in its image. Full per-board pinout:
README "Pinout" + `docs/wiring-*.svg`.

## GPIO map

| GPIO | Owner | Notes |
| --- | --- | --- |
| 6, 8 | C5 ↔ bridge UART | TX 6, RX 8 (frozen by the carrier PCB, `test/test_pcb.py`; GPIO 15 is not broken out on the C5-Zero) |
| 13, 14 | USB-Serial-JTAG (console, flashing) | forbidden |
| 16–22 | SPI flash | forbidden |
| 26 | antenna switch (C5-Zero): high IPEX, low onboard | never driven by default; only `CONFIG_CHECKPOINT_ANTENNA_IPEX` or `K` `i`, C5-Zero only |
| 27 | C5-Zero onboard WS2812 | free since the dump engine uses no GPIO; strapping pin; the detector still has no LED (follow-up) |
| 28 | BOOT button | input, strapping |
| 0, 1, 3–5, 9–12, 23, 24 | free | |
| 2, 7, 25 | free, strapping | free after boot; no external pull or load that holds them at reset |

GDMA: the detector uses none (the engine polls the dump with the CPU).

## Detector (`components/pass_detector`, pure C)

Input: the band power of R1..R8 (slots 0..7) in 0.25 dB codes (dBFS × 4), ~500
readings per second per channel, each with its own µs time, as a rise in dB above each
channel's own rest (`scan_detector.c`, `sd_feed()`). Smoothing is time-based: a median
of 3, then an average whose `alpha` is the weight per 17.5 ms reference sweep
(τ = −17.5 ms / ln(1 − α), 61 ms at 0.25); the rests follow the smoothed level and never
move on a saturated capture; after a gap over 100 ms a channel waits for a full median
again, then its average resumes (re-primes after a gap over `stale_ms`). Each hop goes
through the pair ghost guard (`sd_feed_hop()`, `CONFIG_CHECKPOINT_PAIR_GUARD`, default y): on
a saturated capture the reading of a channel 15 dB under its partner (smoothed) is held, as
the rails' products ring ghosts on it; after a jump into the rails (a VTX switched on beside the
antenna) the source's partner, named by the last 3 readings (docs/detector.md "Pair ghost
guard"). The pass time on the
wire is the smoothed peak (the console's `PASS` line adds a delay estimate). States
`IDLE → RISING → PEAK_PASSED → COOLDOWN`; re-arm needs the refractory period
elapsed AND either the level under `active_db - hysteresis_db` or its 1 s average the
re-arm drop D (`rearm_db`, a runtime parameter: 16 dB by default from
`CONFIG_CHECKPOINT_REARM_DROP_DB`, 0 = off) under the highest level since the pass
(the relative re-arm, for a drone that stays up between laps; the next pass must then rise
that much over the lowest 1 s average since: docs/detector.md "Relative re-arm"). The glue is
`components/checkpoint`; the parameters (alpha, active, hysteresis, drop, min peak,
refractory, stale, D) are editable from the console (`K`+key:
`components/checkpoint/cp_console.c` and `checkpoint.c`; D is `e`/`E`) and over the link
(the bridges' `CONFIG_SET`, `lp_config_t`, echoed as `CONFIG_STATE`; the degraded C3's page
sets them all, D with an info bubble; the C6 keeps one D for every gate, its admin sets it in
the advanced view), saved in NVS namespace
`checkpoint` key `prefs` (`checkpoint_prefs_t`, `PREFS_VERSION` 4, a version-3 record read
with the build's D — bump it on a layout or meaning change). Parameter table (defaults +
console keys): **`docs/detector.md`**.

## Status LED

A board's onboard WS2812 shows state via `components/status_led` (pure-C
state→colour core, host-tested; RMT WS2812 driver; boot red→green→blue
self-test; priority-1 task; ~15%). Onboard GPIO: node **10**, master **8**, both in RGB byte
order (`CONFIG_STATUS_LED_RGB_ORDER`), not GRB
(C5-as-master **27**). **The detector has no status LED**: the C5-Zero's WS2812
(**GPIO27**) is free since the dump engine uses no GPIO, but `status_led` is not in the
detector's build and the checkpoint glue has no LED code (follow-up); a C3 SuperMini has
no WS2812 either. Full scheme (colours per state and role): **`docs/status-led.md`**.

## Fleet (C6 master + C3 nodes)

Race timing across ordered gates (each gate = a C5). The **C6** (`firmware/master`,
esp32c6) is UART-wired to its own C5 (gate 0) and aggregates remote gates over
**ESP-NOW**; a remote gate is a C5 UART-wired to a **C3** (`firmware/node`).
Protocols (`link_proto` v6, `fleet_proto` v6; reflash every board together after a change):
`components/link_proto` (C5↔bridge UART: config, passes, `STATS` with the 8 levels, the 8
detector states (2 bits each, `lp_state_of()`) and the C5's health (CAL_NEEDED = an LO's
DC or IQ out of range or its captures failing, no engine or a stalled scan; faults =
capture faults), `LP_MSG_COMMAND` calibrate/reboot, `GAIN` (the web forces the RX gain, calibrations' VTX guard), `LOG`/`KEY`/`DEBUG` for the C3 and C6 debug pages),
`components/fleet_proto` over `components/espnow_link` (pairing, beacons, `CONFIG`
carrying the detector settings with D, `GATE_PASS`, `NODE_STATUS` relaying the states, the
health and the C5's echoed D, `FP_MSG_COMMAND`, `DEBUG`/`KEY`/`LOG_REQ`/`LOG_CHUNK`: the C6
admin's Debug panel pulls a remote gate's console, one gate at a time, leased),
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

C6 pins are checked against its datasheet (GPIO 4/5 strap only the SDIO edges) and frozen
by the carrier boards (`hardware/README.md`): a default changed here means a new board, and
`test/test_pcb.py` fails until then.
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
(`detector-ipex`, `detector-xiao-c5`, `detector-wave-c5-lcd`, `master-wave-c5`); each variant is an
overlay on the role's `sdkconfig.defaults` and builds in its own dir. flash/monitor fall back to esptool / a serial terminal on the
already-built image when ESP-IDF is not installed (build it elsewhere, flash
here). `make web-master` / `make web-node` (`tools/web_mock.py`) serve the C6/C3
pages from disk against a simulated `/api`, so a page edit needs no build or
flash; a new `/api/state` field or a changed POST buffer goes in the mock too
(`test/test_web_mock.py` fails otherwise).

After editing `firmware/detector/sdkconfig*.defaults`, run `tools/fw.sh clean` (`make
clean`): it deletes every build dir and generated `sdkconfig`/`sdkconfig.<variant>` of the
`make variants` list, never a `*.defaults`. ESP-IDF never reapplies the defaults, and
sdr_rx's CMake stops on a stale one (cert-test archive or `-Og`). The raw commands:

```sh
. ~/esp/esp-idf-v6.0.2/export.sh
( cd firmware/detector && idf.py build )  # esp32c5 → detector-c5.bin (0 warnings)
( cd firmware/node && idf.py build )      # esp32c3 → node-c3.bin
( cd firmware/master && idf.py build )    # esp32c6 → master-c6.bin
cd test     # host tests
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/sdr_rx -I../components/sdr_rx/include test_sdr_dsp.c \
  ../components/sdr_rx/sdr_psd.c ../components/sdr_rx/sdr_level.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/sdr_rx -I../components/sdr_rx/include test_sdr_plan.c \
  ../components/sdr_rx/sdr_plan.c -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include \
  test_pass_detector.c ../components/pass_detector/pass_detector.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include test_scan_detector.c \
  ../components/pass_detector/scan_detector.c ../components/pass_detector/pass_detector.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include -I../components/sdr_rx \
  -I../components/sdr_rx/include test_pair_ghost.c ../components/pass_detector/scan_detector.c \
  ../components/pass_detector/pass_detector.c ../components/sdr_rx/sdr_psd.c ../components/sdr_rx/sdr_level.c \
  ../components/sdr_rx/sdr_plan.c -lm -o /tmp/t && /tmp/t
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
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/checkpoint test_hop_stats.c -lm -o /tmp/t && /tmp/t
node test_web_auth.mjs   # auth.js (page side of web_auth)
node test_web_bubble.mjs # the C3/C6 pages' info bubbles (one snippet; mouse, touch, keyboard)
python3 test_web_mock.py # tools/web_mock.py against the firmware's /api/state and buffers
python3 test_pcb.py      # hardware/ boards against the firmware's Kconfig pin defaults
python3 test_scan_analyze.py   # tools/scan_analyze.py on synthetic recordings
python3 test_flash_tool.py     # tools/flash.py: download key, --check, logs
```

CI (`.github/workflows/checkpoint.yml`) runs the host tests and builds all three
firmwares. Sound: `python3 tools/coin_sound.py` (needs `assets/coin_source.mp3`, not
versioned).
