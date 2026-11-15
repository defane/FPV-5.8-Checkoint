/* Per-channel measurements from one PSD. Target independent. GPL-3.0-or-later.
 *
 * PSD convention (see psd.h): P[k] in natural order, k = 0 is -fs/2, center bin
 * nfft/2 is the LO. Bins are integers; P[k] * scale is tone-normalised power: a
 * full-scale complex tone puts 1.0 (0 dBFS) in its bin. enbw_bins converts a
 * bin sum into band power. The per-bin work is integer (the C5 has no FPU);
 * floats appear only once per channel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Ported to FPV 5.8 Checkpoint from widscan58 by CapnBry (https://github.com/CapnBry/widscan58),
 * commit a9fff97, firmware/src/scan/metrics.h.
 * Changes for FPV 5.8 Checkpoint: only the log tables, metrics_code_q8, metrics_db and the band
 * level of metrics_level (RSSI only), on a precomputed bin window and in signed 0.25 dB codes
 * without the byte code's +255 (sdr_band_level_qdb); no metrics_channel, averages, centroid,
 * OBW, floor, percentile or SNR.
 * This file keeps its GPL-3.0-or-later notice; the repository as a whole is GPL-3.0-only.
 *
 * Pure C (host-tested in test/test_sdr_dsp.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sdr_plan.h"
#include "sdr_psd.h"
#include "sdr_types.h"

/* log2(1 + i/256) - i/256, in 1/65536: filled by metrics_init(). */
extern uint16_t metrics_log2_dev[257];
void metrics_init(void);

/* 40 log10(v) * 256: four times the dB of a power v, i.e. level codes
 * (0.25 dB steps) in 1/256. Integer only; v = 0 gives INT32_MIN / 2. */
static inline int32_t metrics_code_q8(uint64_t v) {
    if (!v) return INT32_MIN / 2;
    int e = 63 - __builtin_clzll(v);
    uint32_t m = e >= 16 ? (uint32_t)(v >> (e - 16)) & 0xffffu : (uint32_t)(v << (16 - e)) & 0xffffu;
    uint32_t i = m >> 8, f = m & 0xffu;
    int32_t d0 = metrics_log2_dev[i], d1 = metrics_log2_dev[i + 1];
    int64_t log2_q16 = (int64_t)e * 65536 + (int32_t)m + d0 + (((d1 - d0) * (int32_t)f) >> 8);
    return (int32_t)((log2_q16 * 49321 + (1 << 19)) >> 20); /* 40 log10(2) * 256 / 65536 = 49321 / 2^20 */
}

/* 10 log10(linear) to about 1e-4 dB without log10f: soft-float log10f costs
 * thousands of cycles on the C5, and every hop needs ~20 of these. log2 of
 * the mantissa comes from the table above, interpolated. */
static inline float metrics_db(float linear) {
    if (!(linear > 1e-20f)) return -200.0f;
    uint32_t u;
    memcpy(&u, &linear, sizeof(u));
    int32_t e = (int32_t)(u >> 23) - 127;
    uint32_t m = u & 0x7fffffu, i = m >> 15, f = m & 0x7fffu;
    int32_t d0 = metrics_log2_dev[i], d1 = metrics_log2_dev[i + 1];
    int32_t log2_q16 = e * 65536 + (int32_t)(m >> 7) + d0 + (((d1 - d0) * (int32_t)f) >> 15);
    return (float)log2_q16 * (3.0102999566f / 65536.0f);
}

/* Once per PSD: the code of a band power of one unit of p, in 1/256 qdb:
 * 4 dB(scale / enbw_bins) * 256 (widscan58's level_q8 without its +255). */
int32_t sdr_level_scale_q8(const psd_result_t *psd);
/* The power of the bins in band, in 0.25 dB steps (dBFS x 4: 0 = a full-scale complex tone):
 * floor((metrics_code_q8(sum) + scale_q8 + 128) / 256). Invalid (DC guard) bins are filled with
 * the window's mean (SDR_RD_DC_MASKED); a capture with an ADC rail hit sets SDR_RD_SATURATED.
 * An empty window or a zero sum gives SDR_LEVEL_NONE. flags may be NULL. */
int16_t sdr_band_level_qdb(const psd_result_t *psd, sdr_band_t band, int32_t scale_q8, uint8_t *flags);
