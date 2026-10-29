# FPV 5.8 Checkpoint

A C5VRX-5 derivative (upstream: https://github.com/Twotoz/C5VRX, remote
`upstream`) on a **Waveshare ESP32-C5-Zero** that adds a "drone just passed"
detector and a coin sound. **The only output is the sound**: no video
(`CONFIG_C5VRX_VIDEO_OUTPUT=n` in `sdkconfig.defaults`; the transport runs
unchanged but `s_dac_gpio` routes it to no pin). Read `AGENTS.md` first: it is upstream's agent
policy (attribution, realtime invariants, sources) and applies here too.
Full feature notes and the bench procedure: `docs/CHECKPOINT.md`.

## Priorities

- The receive chain comes first: MODEM_DIAG, BitScrambler, PARLIO, AGC
  (`main/rf.c`, `main/transport.c`, `main/receiver.c`, `main/gain.c`,
  `main/agc.c`, `main/*.bsasm`) are **not modified** without the
  operator's approval. The checkpoint code only reads receiver state.
- Every checkpoint task stays at priority 1, below the receiver tasks
  (gain_fast 4, control 3, gain_obs 3).
- RAM is tight: ~2 KB free at the boot peak without PSRAM. Checkpoint
  state is heap-allocated after the splash (`checkpoint_start()` in
  `main/main.c`); do not add static `.bss` buffers.
- Conventional Commits, one commit per feature.

## Wiring

MAX98357A I2S amplifier, 5 V supply, 4 Ω speaker. As wired today:
GPIO3 → BCLK, GPIO4 → LRC (WS), GPIO5 → DIN, GPIO1 → SD (HIGH = on).
**This conflicts with the IQ loopback** (`RF_IQ_GPIO_NUMS = {1, 0, 25, 2,
27, 5, 3, 4}`, `main/rf.h`, driven at 40 MS/s, needed for the C/N even
without video): the amplifier must move, e.g. to the freed DAC pins
(BCLK 23, LRC 24, DIN 11, SD 12), before audio is enabled. Audio is off by default
(`CONFIG_AUDIO_PLAYER_ENABLE=n`); pins are set in menuconfig
("Checkpoint audio") and checked at boot by `main/checkpoint.c:pin_owner()`.

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
| 6, 15 | free | |

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

`K l` live trace, `K 0` defaults, `P` play, `p` mute; saved in NVS
`checkpoint` (`checkpoint_prefs_t`, bump `PREFS_VERSION` on layout change).

## Build and tests

```sh
. ~/esp/esp-idf-v6.0.2/export.sh && idf.py build    # must stay at 0 warnings
cd test && cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include \
  test_pass_detector.c ../components/pass_detector/pass_detector.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/audio_player/include test_audio_resample.c -lm -o /tmp/t && /tmp/t
```

CI (`.github/workflows/c5vrx5.yml`) runs both tests. Sound:
`python3 tools/coin_sound.py` (needs `assets/coin_source.mp3`, not
versioned). `test/test_manual_calibration_runtime.py` already fails upstream
with recent host gcc (`-Wformat-truncation` in `dco_slots.h`).
