# Assets

The C5VRX name and logos are not under the GPL (see `BRANDING.md`), so this derivative
carries no C5VRX logo or photo.

## Checkpoint coin sound

| | |
| --- | --- |
| Source file | `assets/coin_source.mp3` (not versioned, see `.gitignore`) |
| Title | 8-Bit Retro Coin |
| Author | KRNBEATZ |
| Origin | Pixabay (search "8-Bit Retro Coin" by KRNBEATZ) |
| Licence | [Pixabay Content License](https://pixabay.com/service/license-summary/) |

Only its generator (`tools/coin_sound.py`) is versioned: the Pixabay Content
License is not the GPL and does not allow redistributing the sound on its own,
so neither the MP3 nor the converted PCM
(`components/audio_player/sounds/coin.pcm`) is committed (both are
gitignored). Each builder generates it locally:

1. download the sound from Pixabay and save it as `assets/coin_source.mp3`;
2. run `python3 tools/coin_sound.py` (needs `ffmpeg` on the PATH); the script
   prints the PCM's SHA-256, duration and peak level. Do not commit it.

The script decodes to mono 22 050 Hz, trims silence below -50 dBFS at both
ends, adds a 2 ms fade-in and a 10 ms fade-out, peak-normalises to -3 dBFS
and writes raw signed 16-bit little-endian PCM. `--check` verifies an
existing PCM without ffmpeg.
