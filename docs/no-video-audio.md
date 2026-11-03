# No video, no sound (detector)

The detector (C5, `firmware/detector`) has no local output at all: no picture,
no sound, no status LED. Every pass goes out over the UART link to its bridge (a
C3 node or the C6 master), and the coin sound is played centrally by the C6
master ([fleet.md](fleet.md)).

## No video output

`firmware/detector/sdkconfig.defaults` sets `CONFIG_C5VRX_VIDEO_OUTPUT=n`
(*C5VRX receiver* menu). The transport (PARLIO TX + BitScrambler) runs exactly as
before, but its bits reach no pin (`s_dac_gpio` is -1 in
`components/c5vrx_rx/transport.c`). The I/Q capture, the C/N, the gain and
`FIELD_DIAG` are unchanged. The on-screen menu, boot logo and DAC test are not
part of this fork at all (`components/c5vrx_rx/menu_stub.c` keeps only the splash
window and the TX ownership the receive chain relies on). After changing
`firmware/detector/sdkconfig.defaults`, delete `firmware/detector/sdkconfig`
before rebuilding.

## No amplifier, no LED

`components/checkpoint` only reports passes; it has no audio or LED code.
`sdkconfig.defaults` still forces `CONFIG_AUDIO_PLAYER_ENABLE=n` and
`CONFIG_STATUS_LED_ENABLE=n`, because the shared components directory builds
those components into every firmware, the detector included.

The only wires on a detector are its power and the crossed UART link to the
bridge (**TX GPIO6, RX GPIO8**, common ground; [wiring-node.svg](wiring-node.svg)).
Never wire anything to the MODEM_DIAG → PARLIO I/Q loopback
(`RF_IQ_GPIO_NUMS = {1, 0, 25, 2, 27, 5, 3, 4}` in `components/c5vrx_rx/rf.h`):
the C/N is computed from those samples, and driving them destroys reception.
GPIO27 is also the C5-Zero's onboard WS2812, which is why the detector has no
status LED.
