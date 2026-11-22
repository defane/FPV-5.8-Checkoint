# No local output (detector)

The detector (C5, `firmware/detector`) has no local output at all: no picture,
no sound, no status LED. Every pass goes out over the UART link to its bridge (a
C3 node or the C6 master), and the coin sound is played centrally by the C6
master ([fleet.md](fleet.md)).

## No amplifier, no LED

`components/checkpoint` only reports passes; it has no audio or LED code. The
detector builds only its `main` component's dependency closure
(`set(COMPONENTS main)` in `firmware/detector/CMakeLists.txt`), so neither
`audio_player` nor `status_led` is in its image.

The only wires on a detector are its power and the crossed UART link to the
bridge (**TX GPIO6, RX GPIO8**, common ground; [wiring-node.svg](wiring-node.svg)).
The sdr_rx engine needs no pin: it reads the PHY's internal I/Q dump, polled by the
CPU. GPIO 26, the C5-Zero's antenna switch, is driven only on request
([bench.md](bench.md#6-gain-and-antenna)). GPIO27, the C5-Zero's onboard WS2812, is
free; the detector does not drive it (a follow-up).

History: the C5VRX engine this project ran before issue #2 also drove a video
transport that reached no pin, and a GPIO-matrix I/Q loopback on GPIO 0–5, 25 and
27, which is why GPIO27's LED was unusable then. Both went with that engine.
