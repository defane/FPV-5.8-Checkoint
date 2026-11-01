# Checkpoint: "a drone just passed"

An addition to C5VRX-5: a pass detector on the **tuned channel** plus an 8-bit
coin sound played at a per-channel pitch (R1..R8, major pentatonic, −7 to +9
semitones, R4 = the original pitch).

| Part | Files |
| --- | --- |
| Detector, pure C | `components/pass_detector/` (test: `test/test_pass_detector.c`) |
| I2S audio player | `components/audio_player/` (test: `test/test_audio_resample.c`) |
| Glue (detection + sound + link) | `components/checkpoint/checkpoint.c`, `checkpoint_link.c` |
| Console | `components/c5vrx_rx/console.c` (upstream C5VRX engine) |
| C5↔C3 UART protocol | `components/link_proto/` (test: `test/test_link_proto.c`) |
| C3 config/telemetry bridge | `firmware/node/` (Wi-Fi SoftAP + web) |
| Sound | `tools/coin_sound.py` → `components/audio_player/sounds/coin.pcm` (`assets/README.md`) |
| A/B/A bench | `tools/checkpoint_bench.py` |

## Pages

- **[Detector: operation and parameters](detector.md)** — the C/N signal, the
  state machine, the `K`-console parameters (saved in NVS).
- **[No video, optional audio](no-video-audio.md)** — sound-only output, the
  pins freed with video off, enabling audio after rewiring.
- **[Race-timing fleet (C6 master)](fleet.md)** — C6 + C3 nodes: protocols,
  pairing, web roles, degraded mode, C5-as-master.
- **[Status LED](status-led.md)** — the per-board WS2812, state ↔ colour scheme.
- **[Bench procedure](bench.md)** — build/flash, signal source, detector and
  audio tests, A/B/A receive-quality check.

## Two-ESP platform

The C5 (`firmware/detector`) does the whole C5VRX receive chain, the detection
and the sound. The C3 (`firmware/node`) serves a Wi-Fi access point and a web
page that tune the detector and show its live C/N, stats and passes, over a UART
link (`components/link_proto`, GPIO 6/8 on the C5 side). The C3 is
**authoritative** for the config: it keeps it in NVS, adopts it from the C5 on
first boot if it has none, and pushes it back on every `HELLO` from the C5 (so
after a C5 reboot). The C5 emits one `EVENT_PASS` per pass and periodic `STATS`.

Wiring diagram: [`wiring.svg`](wiring.svg). Regenerable Graphviz source:
[`wiring.dot`](wiring.dot) (`dot -Tpng docs/wiring.dot -o docs/wiring.png`).
Consolidated pinout: [README](../README.md#pinout).
