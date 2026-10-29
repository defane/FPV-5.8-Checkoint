/* Host test of the audio player's pure helpers (audio_resample.h): the
 * pentatonic pitch table, the linear-interpolation resampler and the gain.
 *
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/audio_player/include test_audio_resample.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "audio_resample.h"

static unsigned s_fail, s_run;
static void check(const char *name, int ok)
{
    ++s_run;
    printf("%-4s %s\n", ok ? "ok" : "FAIL", name);
    s_fail += !ok;
}

#define N 1000u
static uint8_t s_pcm[2u * N + 1u];   /* +1: start the PCM at an odd address */

static size_t render_all(const uint8_t *pcm, uint32_t samples, uint32_t step, int32_t gain, int16_t *out, size_t cap)
{
    audio_voice_t v;
    audio_voice_start(&v, pcm, samples, step, gain);
    size_t total = 0, n;
    while ((n = audio_voice_render(&v, out + total, cap - total > 64 ? 64 : cap - total)) != 0) total += n;
    return total;
}

int main(void)
{
    int table_ok = 1, rising = 1, pentatonic = 1;
    for (unsigned k = 0; k < 8u; ++k) {
        const double want = pow(2.0, audio_pentatonic_semitones[k] / 12.0) * 65536.0;
        table_ok &= fabs(audio_pentatonic_q16[k] - want) <= 0.5;
        if (k) rising &= audio_pentatonic_q16[k] > audio_pentatonic_q16[k - 1];
        const int pc = ((audio_pentatonic_semitones[k] - audio_pentatonic_semitones[0]) % 12 + 12) % 12;
        pentatonic &= pc == 0 || pc == 2 || pc == 4 || pc == 7 || pc == 9;
    }
    check("pitch table = round(2^(n/12) x 65536)", table_ok);
    check("R1..R8 strictly rising, all on one major pentatonic scale", rising && pentatonic);
    check("R4 plays the recording as is; channel index wraps per band", audio_pitch_for_channel(3) == AUDIO_PITCH_ONE &&
          audio_pitch_for_channel(11) == audio_pitch_for_channel(3));
    check("gain: 100 % unity, 60 % default, clamps above 100 %", audio_gain_q15(100) == 32768 &&
          audio_gain_q15(60) == 19660 && audio_gain_q15(250) == 32768 && audio_gain_q15(0) == 0);

    uint8_t *pcm = s_pcm + 1;   /* misaligned on purpose */
    for (unsigned k = 0; k < N; ++k) {
        const int16_t s = (int16_t)(10000.0 * sin(k * 0.05));
        pcm[2 * k] = (uint8_t)((uint16_t)s & 0xFF);
        pcm[2 * k + 1] = (uint8_t)((uint16_t)s >> 8);
    }
    static int16_t out[4u * N];
    size_t n = render_all(pcm, N, AUDIO_PITCH_ONE, audio_gain_q15(100), out, 4u * N);
    int same = n == N;
    for (unsigned k = 0; same && k < N; ++k) same &= out[k] == (int16_t)(10000.0 * sin(k * 0.05));
    check("pitch 1.0, gain 100 %: bit-exact copy from a misaligned buffer", same);
    n = render_all(pcm, N, 2u * AUDIO_PITCH_ONE, audio_gain_q15(100), out, 4u * N);
    check("one octave up: half the samples", n == N / 2u);
    n = render_all(pcm, N, AUDIO_PITCH_ONE / 2u, audio_gain_q15(100), out, 4u * N);
    int mid = n == 2u * N;
    for (unsigned k = 0; mid && k + 2u < 2u * N; k += 2u) mid &= abs(out[k + 1] - (out[k] + out[k + 2]) / 2) <= 1;
    check("one octave down: twice the samples, odd ones interpolated midway", mid);
    n = render_all(pcm, N, audio_pitch_for_channel(7), audio_gain_q15(100), out, 4u * N);
    check("R8 (+9 semitones) length = N / 1.68", labs((long)n - lround(N / 1.681793)) <= 1);
    n = render_all(pcm, N, AUDIO_PITCH_ONE, audio_gain_q15(60), out, 4u * N);
    int scaled = 1;
    for (unsigned k = 0; k < n; ++k) scaled &= abs(out[k] - (int)lround(0.6 * (int16_t)(10000.0 * sin(k * 0.05)))) <= 1;
    check("60 % volume scales every sample", scaled);
    const uint8_t extremes[4] = {0xFF, 0x7F, 0x00, 0x80};   /* 32767, -32768 */
    n = render_all(extremes, 2, AUDIO_PITCH_ONE / 4u, audio_gain_q15(100), out, 16);
    int bounded = n == 8u && out[0] == 32767 && out[4] == -32768;
    check("full-scale samples and interpolation never wrap", bounded);
    n = render_all(pcm, 0, AUDIO_PITCH_ONE, audio_gain_q15(100), out, 16);
    check("empty sound renders nothing", n == 0u);

    printf("%u/%u audio resample checks passed\n", s_run - s_fail, s_run);
    return s_fail != 0;
}
