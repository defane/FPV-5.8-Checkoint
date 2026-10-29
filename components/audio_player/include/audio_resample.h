/* Pure helpers of the audio player (host-tested): per-channel pitch on a
 * pentatonic scale and a linear-interpolation resampler with a digital gain
 * over raw little-endian s16 PCM, read byte by byte (EMBED_FILES data has no
 * alignment guarantee). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define AUDIO_PITCH_ONE 65536u   /* Q16: 1.0 = the recorded pitch */

/* Major pentatonic from 7 semitones under the recording: R1..R8 =
 * -7, -5, -3, 0, +2, +5, +7, +9 semitones (R4 plays the sound as recorded).
 * round(2^(n/12) * 65536). */
static const uint32_t audio_pentatonic_q16[8] = {43740u, 49097u, 55109u, 65536u, 73562u, 87480u, 98193u, 110218u};
static const int8_t audio_pentatonic_semitones[8] = {-7, -5, -3, 0, 2, 5, 7, 9};

static inline uint32_t audio_pitch_for_channel(unsigned channel) { return audio_pentatonic_q16[channel % 8u]; }

/* 0..100 % -> Q15 gain (100 % = unity). */
static inline int32_t audio_gain_q15(unsigned percent) { return (int32_t)((percent > 100u ? 100u : percent) * 32768u / 100u); }

typedef struct {
    const uint8_t *pcm;      /* s16le */
    uint32_t samples;
    uint32_t index, frac;    /* read position: index + frac / 65536 */
    uint32_t step_q16;       /* pitch: source samples per output sample */
    int32_t gain_q15;
} audio_voice_t;

static inline void audio_voice_start(audio_voice_t *v, const uint8_t *pcm, uint32_t samples, uint32_t step_q16,
                                     int32_t gain_q15)
{
    *v = (audio_voice_t){.pcm = pcm, .samples = samples, .step_q16 = step_q16 ? step_q16 : AUDIO_PITCH_ONE,
                         .gain_q15 = gain_q15};
}

static inline int32_t audio_pcm_at(const uint8_t *pcm, uint32_t k)
{
    return (int16_t)(uint16_t)(pcm[2u * k] | (uint16_t)pcm[2u * k + 1u] << 8);
}

/* Up to n output samples; returns how many (0 = the voice has ended). The
 * sample after the last one is silence, so the tail interpolates to zero. */
static inline size_t audio_voice_render(audio_voice_t *v, int16_t *out, size_t n)
{
    size_t k = 0;
    while (k < n && v->index < v->samples) {
        const int32_t s0 = audio_pcm_at(v->pcm, v->index);
        const int32_t s1 = v->index + 1u < v->samples ? audio_pcm_at(v->pcm, v->index + 1u) : 0;
        const int32_t s = s0 + (int32_t)(((int64_t)(s1 - s0) * (int64_t)v->frac) >> 16);
        int32_t y = (int32_t)(((int64_t)s * v->gain_q15) >> 15);
        out[k++] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        v->frac += v->step_q16;
        v->index += v->frac >> 16;
        v->frac &= 0xFFFFu;
    }
    return k;
}
