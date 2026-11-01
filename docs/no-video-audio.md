# No video, optional audio

## No video output

The project produces sound only: `firmware/detector/sdkconfig.defaults` sets
`CONFIG_C5VRX_VIDEO_OUTPUT=n` (*C5VRX receiver* menu). The transport (PARLIO TX +
BitScrambler) runs exactly as before, but its bits reach no pin (`s_dac_gpio` is
-1 in `components/c5vrx_rx/transport.c`). The I/Q capture, the C/N, the gain and
`FIELD_DIAG` are unchanged. You lose the picture, the on-screen menu and the DAC
test (`Z`). The six DAC pins (8, 9, 11, 12, 23 and 24) plus 7 and 10 become free
for the amplifier. After changing `firmware/detector/sdkconfig.defaults`, delete
`firmware/detector/sdkconfig` before rebuilding.

## Audio: disabled by default

`CONFIG_AUDIO_PLAYER_ENABLE=n`. The first amplifier wiring (BCLK on GPIO3, LRC on
GPIO4, DIN on GPIO5, SD on GPIO1) sat on the MODEM_DIAG → PARLIO I/Q loopback
(`RF_IQ_GPIO_NUMS = {1, 0, 25, 2, 27, 5, 3, 4}` in `components/c5vrx_rx/rf.h`).
Driving those pins would destroy reception. **Unwire the amplifier from them.**

To enable audio after rewiring: `idf.py menuconfig` → *Checkpoint audio
(MAX98357A I2S)*. Enable the option and enter the pins. The firmware refuses to
start audio on a receiver pin and says so with a `CHECKPOINT audio=refused` line.
With video off, the usable pins are 8, 9, 11, 12, 23 and 24, plus 7 and 10 (6 and
8 carry the UART link to the C3). Chosen wiring (Kconfig defaults): **BCLK on
GPIO9, LRC on GPIO8, DIN on GPIO7, SD on GPIO10**. SD is refused on GPIO7 (a
strapping pin). The I/Q GPIOs (0, 1, 2, 3, 4, 5, 25 and 27) stay forbidden: the
C/N is computed from those samples. `coin.pcm` is also required
(`assets/README.md`).

The I2S channel — and thus a GDMA TX channel — only exists while sounds play.
`transport.c:quiet_interrupts()` turns off all three GDMA channels' interrupts on
every transport restart (boot, menu exit, OUTPUT/profile/demodulator change,
recovery). A permanent I2S channel would stay stuck; one recreated per burst of
sounds re-arms its interrupts. If a restart lands during a sound, the sound is
cut (`AUDIO error=write`, `failed` counter) and the next one plays normally.
