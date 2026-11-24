/* Welch PSD: esp-dsp float/fixed FFTs, or a rounded fixed FFT (esp-sdr's on
 * the S3, fft_r4_sc16.h elsewhere).
 * GPL-3.0-or-later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Ported to Lëtz Timer from widscan58 by CapnBry (https://github.com/CapnBry/widscan58),
 * commit a9fff97, firmware/src/dsp/psd.c.
 * Changes for Lëtz Timer: the S16R path only (fft_r4_sc16.h; no esp-dsp, float FFTs,
 * psd_set_fft or psd_release_unused), the buffers in a caller-provided workspace, iq_applied,
 * psd_moments_add/psd_moments_mean/psd_iq_irr_db added; timing fields read 0 on the host.
 * Deviation: a capture with saturated > 0 does not update the psd_iq_t state (the state is still
 * applied): a clipped capture biases the moments (weight ~0.23 per capture) just when the image
 * matters most.
 * This file keeps its GPL-3.0-or-later notice; the repository as a whole is GPL-3.0-only. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sdr_host.h"
#include "sdr_psd.h"
#include "sdr_radio.h"
/* The rounded radix-4 kernel (fft_r4_sc16.h): esp-sdr's X/N output, about half the
 * instructions of its portable radix-2 one, its own twiddles in flash, digit-reversed input and
 * natural-order output. */
#include "fft_r4_sc16.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846 /* glibc hides it under -std=c11 */
#endif

static int16_t *work16;   /* 2 * PSD_MAX_FFT interleaved complex */
static int16_t *window16; /* Q15 Hann */
/* Pass 1 per segment: integer sums. Kept for all segments first, so the IQ
 * correction is worked out once per capture (soft float on the C5 cost
 * ~33 us per segment when done per segment). */
typedef struct {
    int32_t si, sq, peak;
} seg_stats_t;
static seg_stats_t *seg_stats; /* PSD_MAX_SEGMENTS */
/* S16 input scaling is block floating point: each segment is shifted left by
 * the most bits that keep it within half the int16 range (one bit of headroom
 * for complex butterfly growth), then its power is scaled back exactly. Real
 * noise only spans a few ADC counts, so a fixed full-scale factor would leave
 * it in the bottom bits of the 16-bit FFT. Powers are expressed relative to a
 * x64 reference (S16_REF_SHIFT), the factor that maps 10-bit full scale onto
 * int16. The FFT halves every stage, so its output is X/N. */
#define S16_REF_SHIFT 6
#define S16_SCALE (1 << S16_REF_SHIFT)
#define S16_MAX_SHIFT 12
/* Integer power accumulator unit: 2^-(2 * (S16_MAX_SHIFT - S16_REF_SHIFT)) of
 * the reference power at shift 0, scaled up by 2^acc_exp. */
#define S16_UNIT_EXP (2 * (S16_MAX_SHIFT - S16_REF_SHIFT))

static float *window;
static uint32_t *acc;    /* power per bin, in units of the result scale */
static uint8_t *valid;
/* For the radix-4 kernel: input sample -> its position in the work buffer. */
static uint16_t *dest;
static unsigned window_n;
static float window_s1, window_s2;

/* The workspace layout (PSD_WS_BYTES). */
#define WS_WORK16 0u
#define WS_WINDOW16 (WS_WORK16 + 2u * PSD_MAX_FFT * sizeof(int16_t))
#define WS_WINDOW (WS_WINDOW16 + PSD_MAX_FFT * sizeof(int16_t))
#define WS_ACC (WS_WINDOW + PSD_MAX_FFT * sizeof(float))
#define WS_DEST (WS_ACC + PSD_MAX_FFT * sizeof(uint32_t))
#define WS_SEG (WS_DEST + PSD_MAX_FFT * sizeof(uint16_t))
#define WS_VALID (WS_SEG + PSD_MAX_SEGMENTS * sizeof(seg_stats_t))
#define WS_END (WS_VALID + PSD_MAX_FFT)
_Static_assert(WS_END <= PSD_WS_BYTES, "psd workspace");

esp_err_t psd_init(void *ws, size_t bytes) {
    if (!ws || ((uintptr_t)ws & 15u)) return ESP_ERR_INVALID_ARG;
    if (bytes < PSD_WS_BYTES) return ESP_ERR_INVALID_SIZE;
    uint8_t *base = ws;
    work16 = (int16_t *)(base + WS_WORK16);
    window16 = (int16_t *)(base + WS_WINDOW16);
    window = (float *)(base + WS_WINDOW);
    acc = (uint32_t *)(base + WS_ACC);
    dest = (uint16_t *)(base + WS_DEST);
    seg_stats = (seg_stats_t *)(base + WS_SEG);
    valid = base + WS_VALID;
    window_n = 0; /* refill the windows and positions */
    return ESP_OK;
}

static void window_setup(unsigned n) {
    if (window_n == n) return;
    window_s1 = window_s2 = 0;
    for (unsigned j = 0; j < n; j++) {
        float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * j / n);
        window[j] = w;
        window16[j] = (int16_t)lrintf(w * 32767.0f);
        window_s1 += w;
        window_s2 += w * w;
    }
    window_n = n;
    fft_r4_input_pos(dest, n);
}

/* S16 pass 2: remove the mean, IQ correction (Q14 c1, c2: q' = c1 q + c2 i;
 * 1, 0 is exact), Q15 window, optional conjugate, and store at the FFT's
 * input position. Specialised per conj by the caller. */
static inline __attribute__((always_inline)) void window_s16(const uint32_t *seg, unsigned n, int shift,
                                                             int32_t m_i, int32_t m_q, int32_t c1, int32_t c2,
                                                             bool conj) {
    for (unsigned j = 0; j < n; j++) {
        int32_t w = window16[j];
        int32_t ci = (radio_word_i(seg[j]) << shift) - m_i;
        int32_t cq = (radio_word_q(seg[j]) << shift) - m_q;
        cq = (cq * c1 + ci * c2 + 8192) >> 14;
        int32_t vi = (ci * w + 16384) >> 15;
        int32_t vq = (cq * w + 16384) >> 15;
        int16_t *at = work16 + 2 * dest[j];
        at[0] = (int16_t)vi;
        at[1] = (int16_t)(conj ? -vq : vq);
    }
}

/* Correction from the averaged moments: q' = c1 q + c2 i has q's power equal
 * to i's and no correlation with it. Implausible estimates (no signal at all,
 * or far beyond any real imbalance) leave the samples as they are.
 * Returns whether the correction applies. */
static bool iq_coefficients(const psd_iq_t *iq, float *c1, float *c2) {
    *c1 = 1.0f;
    *c2 = 0.0f;
    if (!iq || !(iq->ii > 0.0f) || !(iq->qq > 0.0f)) return false;
    float ratio = sqrtf(iq->qq / iq->ii);     /* Q gain relative to I */
    float rho = iq->iq / sqrtf(iq->ii * iq->qq); /* sin(phase error) */
    if (ratio < 0.7f || ratio > 1.4f || fabsf(rho) > 0.3f) return false;
    float s = sqrtf(1.0f - rho * rho);
    *c1 = 1.0f / (ratio * s);
    *c2 = -rho / s;
    return true;
}
#define IQ_AVG 0.0625f /* per segment: ~4 captures of 4 segments */


/* round(v / 2^bits), half up; bits >= 1. */
static inline int32_t round_shift(int32_t v, unsigned bits) { return (v + (1 << (bits - 1))) >> bits; }

IRAM_ATTR esp_err_t psd_compute(const uint32_t *words, unsigned n, unsigned nfft, unsigned max_segments,
                      float rate_hz, bool conjugate, float usable_hz, unsigned dc_guard,
                      psd_iq_t *iq, psd_result_t *out) {
    if (!acc) return ESP_ERR_INVALID_STATE;
    if (nfft < 64 || nfft > PSD_MAX_FFT || (nfft & (nfft - 1))) return ESP_ERR_INVALID_ARG;
    unsigned segments = n / nfft;
    if (segments > max_segments) segments = max_segments;
    if (segments > PSD_MAX_SEGMENTS) segments = PSD_MAX_SEGMENTS;
    if (!segments) return ESP_ERR_INVALID_SIZE;
    window_setup(nfft);
    memset(acc, 0, nfft * sizeof(*acc));
    const unsigned log2n = 31 - (unsigned)__builtin_clz(nfft);

    /* S16: acc holds integer power in units of 2^acc_exp (see S16_UNIT_EXP).
     * A segment with less input shift (a stronger signal) needs a coarser
     * unit; acc is then rescaled once, so no segment overflows or loses its
     * top bits. One |X/N|^2 is at most 2^29 (|v| < 2^14 per component), so
     * 4 segments fit 32 bits; beyond that, headroom bits are reserved. */
    const unsigned headroom = segments > 4 ? 32 - __builtin_clz(segments - 1) - 2 : 0;
    int acc_exp = -1; /* nothing accumulated yet */

    unsigned saturated = 0;
    int64_t dci = 0, dcq = 0;
    /* Centered moments summed over segments, times nfft: nfft * sum(i^2) -
     * sum(i)^2 per segment (< 2^39 each). */
    int64_t cii = 0, cqq = 0, ciq = 0;
    int64_t t_prep = 0, t_fft = 0, t_power = 0, t = sdr_now_us(), t2;
    const unsigned half = nfft / 2;
    /* Pass 1: unpack, sum for the mean, count rail hits, peak. The peak uses
     * one's-complement magnitudes (v ^ sign): both rails, -512 and 511, map to
     * 511, so one compare finds them. The S16 shift bound below has room for
     * the 1 it can under-read. */
    for (unsigned s = 0; s < segments; s++) {
        const uint32_t *seg = words + s * nfft;
        int32_t si = 0, sq = 0, peak = 0;
        if (iq) {
            /* Products of 10-bit values: 2^18 each, so a segment's sums fit 2^29. */
            int32_t pii = 0, pqq = 0, piq = 0;
            for (unsigned j = 0; j < nfft; j++) {
                int32_t i = radio_word_i(seg[j]), q = radio_word_q(seg[j]);
                si += i;
                sq += q;
                pii += i * i;
                pqq += q * q;
                piq += i * q;
                int32_t ai = i ^ (i >> 31), aq = q ^ (q >> 31);
                int32_t m = ai > aq ? ai : aq;
                saturated += m >= 511;
                if (m > peak) peak = m;
            }
            cii += ((int64_t)pii << log2n) - (int64_t)si * si;
            cqq += ((int64_t)pqq << log2n) - (int64_t)sq * sq;
            ciq += ((int64_t)piq << log2n) - (int64_t)si * sq;
        } else {
            for (unsigned j = 0; j < nfft; j++) {
                int32_t i = radio_word_i(seg[j]), q = radio_word_q(seg[j]);
                si += i;
                sq += q;
                int32_t ai = i ^ (i >> 31), aq = q ^ (q >> 31);
                int32_t m = ai > aq ? ai : aq;
                saturated += m >= 511;
                if (m > peak) peak = m;
            }
        }
        seg_stats[s] = (seg_stats_t){.si = si, .sq = sq, .peak = peak};
        dci += si;
        dcq += sq;
    }
    /* IQ correction, once per capture: the moments' average updates the
     * per-LO state with the weight the per-segment updates would have had
     * together, 1 - (1 - IQ_AVG)^segments. Not from a saturated capture. */
    float c1 = 1.0f, c2 = 0.0f;
    bool iq_applied = false;
    if (iq) {
        if (!saturated) {
            const float k = 1.0f / ((float)segments * (float)nfft * (float)nfft);
            const float ii = (float)cii * k, qq = (float)cqq * k, iqm = (float)ciq * k;
            if (!iq->init) {
                iq->ii = ii;
                iq->qq = qq;
                iq->iq = iqm;
                iq->init = true;
            } else {
                float keep = 1.0f;
                for (unsigned s = 0; s < segments; s++) keep *= 1.0f - IQ_AVG;
                const float a = 1.0f - keep;
                iq->ii += a * (ii - iq->ii);
                iq->qq += a * (qq - iq->qq);
                iq->iq += a * (iqm - iq->iq);
            }
        }
        iq_applied = iq_coefficients(iq, &c1, &c2);
    }
    /* Q14, as window_s16 applies them; the S16 shift bound uses the same
     * values, so it covers the gain the correction really applies. */
    const int32_t k1 = (int32_t)lrintf(c1 * 16384.0f), k2 = (int32_t)lrintf(c2 * 16384.0f);
    const int32_t gain_q14 = abs(k1) + abs(k2) > 16384 ? abs(k1) + abs(k2) : 16384;
    t2 = sdr_now_us(); t_prep += t2 - t; t = t2;

    for (unsigned s = 0; s < segments; s++) {
        const uint32_t *seg = words + s * nfft;
        const seg_stats_t *st = &seg_stats[s];
        /* Pass 2: remove the mean (DC/LO leakage), window, optional conjugate.
         * |v - mean| <= peak + |mean|, and the IQ correction can grow Q by
         * up to |c1| + |c2|; keep that below 2^14 after the shift.
         * Windowed values then stay within int16 without clamping. */
        const int32_t ami = (abs(st->si) + (int32_t)nfft - 1) >> log2n, amq = (abs(st->sq) + (int32_t)nfft - 1) >> log2n;
        const int32_t bound = ((st->peak + (ami > amq ? ami : amq)) * gain_q14 + 16383) >> 14;
        int shift = 0;
        while (shift < S16_MAX_SHIFT && (bound << (shift + 1)) <= 16383) shift++;
        int e = 2 * (S16_MAX_SHIFT - shift) + (int)headroom;
        if (e > acc_exp) {
            if (acc_exp >= 0)
                for (unsigned k = 0; k < nfft; k++) acc[k] >>= (unsigned)(e - acc_exp);
            acc_exp = e;
        }
        const unsigned power_shift = (unsigned)(acc_exp - 2 * (S16_MAX_SHIFT - shift));
        /* The mean at the segment's scale, rounded: sum * 2^shift / nfft.
         * No overflow: |mean| <= peak + 1 and peak << shift < 2^14. */
        const int32_t m_i = shift >= (int)log2n ? st->si << (shift - (int)log2n) : round_shift(st->si, log2n - (unsigned)shift);
        const int32_t m_q = shift >= (int)log2n ? st->sq << (shift - (int)log2n) : round_shift(st->sq, log2n - (unsigned)shift);
        if (conjugate) window_s16(seg, nfft, shift, m_i, m_q, k1, k2, true);
        else window_s16(seg, nfft, shift, m_i, m_q, k1, k2, false);
        t2 = sdr_now_us(); t_prep += t2 - t; t = t2;
        fft_r4_sc16(work16, nfft);
        t2 = sdr_now_us(); t_fft += t2 - t; t = t2;
        for (unsigned k = 0; k < nfft; k++) {
            int32_t re = work16[2 * k], im = work16[2 * k + 1];
            /* Each square fits int32; the sum can reach 2^31, so add unsigned. */
            acc[(k + half) & (nfft - 1)] += ((uint32_t)(re * re) + (uint32_t)(im * im)) >> power_shift;
        }
        t2 = sdr_now_us(); t_power += t2 - t; t = t2;
    }
    /* Full-scale complex tone (amplitude RADIO_FULL_SCALE) -> 1.0 in its bin.
     * S16 output is X/N of an input scaled by S16_SCALE. */
    const float amp = RADIO_FULL_SCALE * S16_SCALE * window_s1 / nfft;
    const float norm = 1.0f / (segments * amp * amp);
    const float scale = ldexpf(norm, acc_exp - S16_UNIT_EXP);
    const float bin_hz = rate_hz / nfft;
    const unsigned usable_bins = (unsigned)(usable_hz / bin_hz);
    for (unsigned k = 0; k < nfft; k++) {
        unsigned rel = (unsigned)abs((int)k - (int)half);
        valid[k] = rel > dc_guard && rel <= usable_bins;
    }
    out->nfft = nfft;
    out->segments = segments;
    out->bin_hz = bin_hz;
    out->enbw_bins = nfft * window_s2 / (window_s1 * window_s1);
    out->saturated = saturated;
    out->dc_i = (float)dci / (float)(segments * nfft);
    out->dc_q = (float)dcq / (float)(segments * nfft);
    out->prep_us = (uint32_t)t_prep;
    out->fft_us = (uint32_t)t_fft;
    out->power_us = (uint32_t)t_power;
    out->p = acc;
    out->scale = scale;
    out->valid = valid;
    out->iq_applied = iq_applied;
    return ESP_OK;
}

unsigned psd_moments_add(const uint32_t *words, unsigned n, unsigned nfft, sdr_moments_t *m) {
    if (!m || nfft < 4 || nfft > 4096 || (nfft & (nfft - 1))) return 0;
    if (m->segments && m->nfft != nfft) return 0;
    const unsigned segments = n / nfft, log2n = 31 - (unsigned)__builtin_clz(nfft);
    int64_t cii = 0, cqq = 0, ciq = 0;
    unsigned saturated = 0;
    for (unsigned s = 0; s < segments; s++) {
        const uint32_t *seg = words + s * nfft;
        /* The same integer sums as psd_compute's pass 1 (2^18 per product: nfft <= 4096 fits). */
        int32_t si = 0, sq = 0, pii = 0, pqq = 0, piq = 0;
        for (unsigned j = 0; j < nfft; j++) {
            int32_t i = radio_word_i(seg[j]), q = radio_word_q(seg[j]);
            si += i;
            sq += q;
            pii += i * i;
            pqq += q * q;
            piq += i * q;
            int32_t ai = i ^ (i >> 31), aq = q ^ (q >> 31);
            saturated += (ai > aq ? ai : aq) >= 511;
        }
        cii += ((int64_t)pii << log2n) - (int64_t)si * si;
        cqq += ((int64_t)pqq << log2n) - (int64_t)sq * sq;
        ciq += ((int64_t)piq << log2n) - (int64_t)si * sq;
    }
    if (saturated) {
        m->saturated++;
        return saturated;
    }
    if (!segments) return 0;
    m->cii += cii;
    m->cqq += cqq;
    m->ciq += ciq;
    m->segments += segments;
    m->nfft = nfft;
    return 0;
}

psd_iq_t psd_moments_mean(const sdr_moments_t *m) {
    psd_iq_t r = {0};
    if (!m || !m->segments || !m->nfft) return r;
    const double k = 1.0 / ((double)m->segments * (double)m->nfft * (double)m->nfft);
    r.ii = (float)((double)m->cii * k);
    r.qq = (float)((double)m->cqq * k);
    r.iq = (float)((double)m->ciq * k);
    r.init = true;
    return r;
}

/* With s = sqrt(1 - rho^2) and t = 2 g rho^2 / (1 + s) = 2 g (1 - s), the ratio is
 * ((1 + g)^2 - t) / ((1 - g)^2 + t): the same formula, without the cancellation near 1. */
float psd_iq_irr_db(const psd_iq_t *iq) {
    if (!iq || !(iq->ii > 0.0f) || !(iq->qq > 0.0f)) return 0.0f;
    const float g = sqrtf(iq->qq / iq->ii);
    float r2 = iq->iq * iq->iq / (iq->ii * iq->qq);
    if (r2 > 1.0f) r2 = 1.0f;
    const float s = sqrtf(1.0f - r2), t = 2.0f * g * r2 / (1.0f + s);
    const float num = (1.0f + g) * (1.0f + g) - t, den = (1.0f - g) * (1.0f - g) + t;
    if (!(den > 0.0f)) return 99.9f;
    const float db = 10.0f * log10f(num / den);
    return db < 0.0f ? 0.0f : db > 99.9f ? 99.9f : db;
}
