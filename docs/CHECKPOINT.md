# Checkpoint: "a drone just passed"

An addition to C5VRX-5: a C5 that sweeps **every Raceband channel** and detects a pass on
any of them, reported
over a UART link; the C6 master plays an 8-bit coin sound at a per-channel pitch
(R1..R8, major pentatonic, −7 to +9 semitones, R4 = the original pitch).

| Part | Files |
| --- | --- |
| Detector, pure C | `components/pass_detector/` (test: `test/test_pass_detector.c`) |
| Glue (detection + link) | `components/checkpoint/checkpoint.c`, `checkpoint_link.c` |
| Console | `components/c5vrx_rx/console.c` (C5VRX engine, pruned key table) |
| C5↔C3 UART protocol | `components/link_proto/` (test: `test/test_link_proto.c`) |
| C3 config/telemetry bridge | `firmware/node/` (ESP-NOW relay, or Wi-Fi SoftAP + web) |
| I2S coin-sound player (C6 master) | `components/audio_player/` (test: `test/test_audio_resample.c`) |
| Sound | `tools/coin_sound.py` → `components/audio_player/sounds/coin.pcm` (`assets/README.md`) |

## Pages

- **[Detector: operation and parameters](detector.md)** — the C/N signal, the
  state machine, the `K`-console parameters (saved in NVS).
- **[No video, no sound (detector)](no-video-audio.md)** — the detector has no
  local output; what it may and may not be wired to.
- **[Race-timing fleet (C6 master)](fleet.md)** — C6 + C3 nodes: protocols,
  pairing, web roles, degraded mode, the centralized sound, C5-as-master.
- **[Status LED](status-led.md)** — the per-board WS2812, state ↔ colour scheme.
- **[Bench procedure](bench.md)** — build/flash, signal source, detector tests.

## Two-ESP platform

The C5 (`firmware/detector`) does the whole C5VRX receive chain and the
detection; it has no sound of its own. The C3 (`firmware/node`) relays to the C6
master over ESP-NOW, or, with no master in reach, serves a Wi-Fi access point and
a web page that tune the detector and show its live C/N, stats and passes, over
a UART link (`components/link_proto`, GPIO 6/8 on the C5 side). The C3 keeps its
own copy of the detector config in NVS and pushes it to the C5 (`CONFIG_SET`)
when it changes: from its web page in degraded mode, or when the C6 master
assigns it (a fleet `CONFIG`). The C5 applies it, saves it in its own NVS and
echoes it (`CONFIG_STATE`); the C3 adopts that echo as the current config, so its
page and its calibration guard follow the C5's real values, and asks for it
(`CONFIG_REQ`) whenever the C5 says `HELLO`, e.g. after a C5 reboot. The C5 emits
one `EVENT_PASS` per pass and periodic `STATS` carrying its health; the bridge can
send it `COMMAND`s (calibrate, reboot): see [fleet.md](fleet.md#gate-health-and-remote-commands-no-console-in-production).

Wiring diagram: [`wiring-node.svg`](wiring-node.svg) (C3 + C5). Consolidated
pinout: [README](../README.md#pinout).
