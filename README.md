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
- **ESP32-C3** (`firmware/c3`) — a Wi-Fi SoftAP + web UI that tunes the detector
  (thresholds, volume, mute) and shows the live C/N, stats and recent passes.
  It is the authoritative config store and talks to the C5 over a UART link.

## Layout

```
components/
  c5vrx_rx/      upstream C5VRX receive engine, reused unchanged as a component
  checkpoint/    pass detection + coin sound + the C5<->C3 UART link
  pass_detector/ pure-C pass detector (host-tested)
  audio_player/  I2S coin-sound player
  link_proto/    pure-C C5<->C3 UART protocol (host-tested)
firmware/c5/     ESP32-C5 application (RF + detection + sound)
firmware/c3/     ESP32-C3 config/telemetry bridge (UART + Wi-Fi + web)
docs/            CHECKPOINT.md (feature notes + bench), wiring.svg, engine research
```

See **[docs/CHECKPOINT.md](docs/CHECKPOINT.md)** for the feature notes and bench
procedure, and **[docs/wiring.svg](docs/wiring.svg)** for the wiring between the
C3, the C5 and the amplifier.

## Build

```sh
. ~/esp/esp-idf-v6.0.2/export.sh
( cd firmware/c5 && idf.py build )     # ESP32-C5, must stay at 0 warnings
( cd firmware/c3 && idf.py build )     # ESP32-C3 bridge

# Host tests
cd test
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include \
   test_pass_detector.c ../components/pass_detector/pass_detector.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/audio_player/include \
   test_audio_resample.c -lm -o /tmp/t && /tmp/t
cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/link_proto/include \
   test_link_proto.c ../components/link_proto/link_proto.c -o /tmp/t && /tmp/t
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
