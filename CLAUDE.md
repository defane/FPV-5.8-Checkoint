# FPV 5.8 Checkpoint

A two-ESP "drone just passed" platform built on C5VRX-5 (upstream:
https://github.com/Twotoz/C5VRX, remote `upstream`):

- **ESP32-C5** (Waveshare ESP32-C5-Zero, `firmware/c5`): the full C5VRX RF
  receive chain → pass detection on the tuned channel's C/N → a coin sound.
  **The only local output is the sound**: no video
  (`CONFIG_C5VRX_VIDEO_OUTPUT=n` in `firmware/c5/sdkconfig.defaults`; the
  transport runs unchanged but `s_dac_gpio` routes it to no pin).
- **ESP32-C3** (`firmware/c3`): a Wi-Fi SoftAP + web UI that tunes the
  detector and shows its live C/N, stats and pass events, over a UART link.

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
  audio_player/  I2S coin-sound player
  link_proto/    pure-C C5<->C3 UART protocol (host-tested)
firmware/c5/     ESP32-C5 app (main.c = app_main consuming the components)
firmware/c3/     ESP32-C3 config/telemetry bridge (UART + Wi-Fi + web)
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
  `firmware/c5/main/main.c`); do not add static `.bss` buffers.
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
| UART RX (← C3 TX) | 15 |

The first amplifier wiring (GPIO3 BCLK, 4 LRC, 5 DIN, 1 SD) sat on the IQ
loopback (`RF_IQ_GPIO_NUMS = {1, 0, 25, 2, 27, 5, 3, 4}`,
`components/c5vrx_rx/rf.h`, driven at 40 MS/s and needed for the C/N even
without video) and was abandoned. Audio is off until `coin.pcm` exists
(`CONFIG_AUDIO_PLAYER_ENABLE=n`); pins are checked at boot by
`components/checkpoint/checkpoint.c:pin_owner()`.

## GPIO map

| GPIO | Owner | Notes |
| --- | --- | --- |
| 0, 1, 2, 3, 4, 5, 25, 27 | IQ loopback (MODEM_DIAG → PARLIO RX) | forbidden; GPIO-matrix loopback, no wiring |
| 8, 9, 11, 12, 23, 24 | 6-bit video DAC (PARLIO TX) | **free with video off** (default here) |
| 7, 10 | reserved 7th/8th DAC branches | free with video off; 7 is a strapping pin (never SD) |
| 13, 14 | USB-Serial-JTAG (console, flashing) | forbidden |
| 16–22 | SPI flash | forbidden |
| 26 | antenna switch (C5-Zero) | forbidden |
| 28 | BOOT button | input, strapping |
| 6, 15 | C5 ↔ C3 UART link | TX 6, RX 15 (only receiver-free pins) |

GDMA: 3 AHB pairs. C5VRX uses 1 RX (PARLIO RX) and 1 TX (PARLIO TX; the TX
BitScrambler is inline, no channel of its own). I2S gets a free TX channel,
created per burst of sounds because `transport.c:quiet_interrupts()` turns
off all three channels' interrupts on every transport restart.

## Detector (`components/pass_detector`, pure C)

Input: the tuned channel's C/N in dB (`receiver_obs_cn_x10()`), every 50 ms.
Channel slot = `rf_channel_index() % 8` (R1..R8 → 0..7).
States `IDLE → RISING → PEAK_PASSED → COOLDOWN`; re-arm needs the refractory
period elapsed AND the level under `active_db - hysteresis_db`.

| Parameter | Default | Console (`K` + key) |
| --- | --- | --- |
| alpha | 0.30 | a / A ±0.05 |
| active_db | 15 dB | t / T ±1 |
| hysteresis_db | 3 dB | h / H ±0.5 |
| drop_db | 3 dB | d / D ±0.5 |
| min_peak_db | 25 dB | m / M ±1 |
| refractory_ms | 1500 | r / R ±250 |
| stale_ms | 1000 | — |
| volume | 60 % | v / V ±10 |

`K l` live trace, `K 0` defaults, `P` play, `p` mute (console in
`components/c5vrx_rx/console.c`); the glue is `components/checkpoint`. The same
settings are editable from the C3 web UI, which pushes them over the link
(`CONFIG_SET`) and the C5 echoes the validated result (`CONFIG_STATE`). Saved
in NVS `checkpoint` (`checkpoint_prefs_t`, bump `PREFS_VERSION` on layout
change).

## Build and tests

```sh
. ~/esp/esp-idf-v6.0.2/export.sh
( cd firmware/c5 && idf.py build )    # C5, must stay at 0 warnings
( cd firmware/c3 && idf.py build )    # C3 bridge
cd test     # host tests (engine headers now at ../components/c5vrx_rx)
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include \
  test_pass_detector.c ../components/pass_detector/pass_detector.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/audio_player/include test_audio_resample.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/link_proto/include \
  test_link_proto.c ../components/link_proto/link_proto.c -o /tmp/t && /tmp/t
```

CI (`.github/workflows/c5vrx5.yml`) builds both firmwares and runs the host
tests (the upstream engine tests compile against `components/c5vrx_rx`). Sound:
`python3 tools/coin_sound.py` (needs `assets/coin_source.mp3`, not versioned).
`test/test_manual_calibration_runtime.py` already fails upstream with recent
host gcc (`-Wformat-truncation` in `dco_slots.h`).
