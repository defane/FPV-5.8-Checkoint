# Assets

Logos and photos in this folder belong to C5VRX (see `BRANDING.md`).

## Checkpoint coin sound

| | |
| --- | --- |
| Source file | `assets/coin_source.mp3` (not versioned, see `.gitignore`) |
| Title | 8-Bit Retro Coin |
| Author | KRNBEATZ |
| Origin | Pixabay (search "8-Bit Retro Coin" by KRNBEATZ) |
| Licence | [Pixabay Content License](https://pixabay.com/service/license-summary/) |

Only the converted PCM (`components/audio_player/sounds/coin.pcm`) and its
generator (`tools/coin_sound.py`) are versioned. To regenerate it:

1. download the sound from Pixabay and save it as `assets/coin_source.mp3`;
2. run `python3 tools/coin_sound.py` (needs `ffmpeg` on the PATH);
3. commit `components/audio_player/sounds/coin.pcm`; the script prints its
   SHA-256, duration and peak level.

The script decodes to mono 22 050 Hz, trims silence below -50 dBFS at both
ends, adds a 2 ms fade-in and a 10 ms fade-out, peak-normalises to -3 dBFS
and writes raw signed 16-bit little-endian PCM. `--check` verifies an
existing PCM without ffmpeg.
