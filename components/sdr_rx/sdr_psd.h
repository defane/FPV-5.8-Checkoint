/* Welch PSD from a raw snapshot. GPL-3.0-or-later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Ported to FPV 5.8 Checkpoint from widscan58 by CapnBry (https://github.com/CapnBry/widscan58),
 * commit a9fff97, firmware/src/dsp/psd.h.
 * Changes for FPV 5.8 Checkpoint: the rounded 16-bit radix-4 FFT only (no esp-dsp, no FFT
 * choice), a caller-provided workspace, iq_applied, plain moment sums and their image rejection
 * (psd_moments_add/mean, psd_iq_irr_db), no IQ update from a saturated capture; esp_err.h only on
 * the target (sdr_host.h), so the host tests build it.
 * This file keeps its GPL-3.0-or-later notice; the repository as a whole is GPL-3.0-only.
 *
 * Pure C (host-tested in test/test_sdr_dsp.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdr_host.h"
#include "sdr_types.h"

#define PSD_MAX_FFT 256 /* CONFIG_SDR_RX_NFFT: 64, 128 or 256 */

/* psd_compute uses at most this many segments (the SEGMENTS limit). */
#define PSD_MAX_SEGMENTS 64

/* The workspace psd_init takes (16-byte aligned): work16 1 KiB, window16 512 B, the float
 * window 1 KiB, acc 1 KiB, dest 512 B, seg_stats 768 B, valid 256 B. On the target it is the
 * RF dump bank's psd region (only the scan task touches it, never during a capture). */
#define PSD_WS_BYTES 5120u

typedef struct {
    unsigned nfft;
    unsigned segments;      /* actually used */
    float bin_hz;
    float enbw_bins;        /* N * sum(w^2) / sum(w)^2 (1.5 for Hann) */
    unsigned saturated;     /* samples at the ADC rails */
    float dc_i, dc_q;       /* mean raw I/Q (LO leakage / DC offset), counts */
    uint32_t *p;            /* nfft bins, natural order; linear power = p[k] * scale */
    float scale;            /* tone-normalised: a full-scale tone's bin -> 1.0 */
    uint8_t *valid;         /* bins usable for metrics */
    bool iq_applied;        /* the IQ correction was applied (its estimate was plausible) */
    uint32_t prep_us, fft_us, power_us; /* time breakdown (0 on the host) */
} psd_result_t;

/* Digital IQ imbalance correction, one state per LO. The I/Q gain ratio and
 * correlation are averaged over segments (each segment's mean removed), the
 * state updated once per capture, and Q is rebuilt orthogonal to I with I's
 * power before the FFT. Valid for proper
 * (circular) signals, which noise, FM video and pairs of independent signals
 * all are. The PHY's own IQ calibration leaves ~20-40 dB of image rejection;
 * this gave 55-58 dB on the C5 (PLAN.md, image rejection).
 * A capture with a sample at an ADC rail leaves the state as it was (it is still applied). */
typedef struct {
    float ii, qq, iq; /* averaged centered moments, counts^2 */
    bool init;
} psd_iq_t;

/* ws: PSD_WS_BYTES or more, 16-byte aligned. ESP_ERR_INVALID_SIZE / _ARG otherwise. */
esp_err_t psd_init(void *ws, size_t bytes);
/* words: packed I/Q (sdr_radio.h). Uses floor(n/nfft) non-overlapping Hann
 * segments (at most max_segments and PSD_MAX_SEGMENTS). Each segment has its
 * mean removed. nfft: 64, 128 or 256.
 * Bins within +/-dc_guard of the LO and beyond +/-usable_hz are marked invalid.
 * iq: IQ correction state for this LO, updated and applied; NULL = off.
 * out->p and out->valid point into the workspace: valid until the next psd_compute. */
esp_err_t psd_compute(const uint32_t *words, unsigned n, unsigned nfft, unsigned max_segments,
                      float rate_hz, bool conjugate, float usable_hz, unsigned dc_guard,
                      psd_iq_t *iq, psd_result_t *out);

/* Pass 1's per-segment centred sums of one capture (floor(n/nfft) segments of nfft, a power of
 * two up to 4096), added to m without any averaging. A capture with a sample at an ADC rail adds
 * nothing and counts in m->saturated. Returns that capture's saturated sample count. Every
 * capture of one sum takes the same nfft (the first sets it; another adds nothing). */
unsigned psd_moments_add(const uint32_t *words, unsigned n, unsigned nfft, sdr_moments_t *m);
/* The mean moments of a sum, as psd_compute averages one capture (k = 1/(segments nfft^2)). */
psd_iq_t psd_moments_mean(const sdr_moments_t *m);
/* Image rejection of an I/Q imbalance, dB: g = sqrt(qq/ii), rho = iq/sqrt(ii qq),
 * IRR = 10 log10((1 + 2g sqrt(1-rho^2) + g^2) / (1 - 2g sqrt(1-rho^2) + g^2)), clamped to
 * [0, 99.9]: 0 when ii or qq <= 0, 99.9 when the denominator is 0. g = 1.2 gives 20.8 dB. */
float psd_iq_irr_db(const psd_iq_t *iq);
