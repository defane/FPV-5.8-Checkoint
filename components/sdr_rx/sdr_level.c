/* GPL-3.0-or-later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Ported to Lëtz Timer from widscan58 by CapnBry (https://github.com/CapnBry/widscan58),
 * commit a9fff97, firmware/src/scan/metrics.c (metrics_init, metrics_level), with the per-hop
 * scale of firmware/src/scan/scanner.c do_levels.
 * Changes for Lëtz Timer: metrics_level's RSSI window rewritten on a precomputed bin
 * window (sdr_band_level_qdb), signed qdb codes; the rest of metrics.c is not ported.
 * This file keeps its GPL-3.0-or-later notice; the repository as a whole is GPL-3.0-only. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sdr_host.h"
#include "sdr_level.h"

uint16_t metrics_log2_dev[257];

void metrics_init(void) {
    for (unsigned i = 0; i <= 256; i++)
        metrics_log2_dev[i] = (uint16_t)lrint((log2(1.0 + i / 256.0) - i / 256.0) * 65536.0);
}

/* floor(v / 256), for negative v too. */
static inline int32_t floor_div256(int32_t v) { return v >= 0 ? v / 256 : -((-v + 255) / 256); }

IRAM_ATTR int32_t sdr_level_scale_q8(const psd_result_t *psd) {
    /* Per hop: the scale in level codes, from the cheap metrics_db. */
    return (int32_t)lrintf(SDR_QDB_PER_DB * (metrics_db(psd->scale) - metrics_db(psd->enbw_bins)) * 256.0f);
}

IRAM_ATTR int16_t sdr_band_level_qdb(const psd_result_t *psd, sdr_band_t band, int32_t scale_q8, uint8_t *flags) {
    uint8_t f = psd->saturated ? SDR_RD_SATURATED : 0u;
    const int last = (int)psd->nfft - 1;
    int r0 = band.r0 < 0 ? 0 : band.r0, r1 = band.r1 > last ? last : band.r1;
    const unsigned total = r1 >= r0 ? (unsigned)(r1 - r0 + 1) : 0;
    uint64_t sum = 0;
    unsigned used = 0;
    for (int k = r0; k <= r1; k++) {
        if (!psd->valid[k]) continue;
        sum += psd->p[k];
        used++;
    }
    /* Masked bins are filled with the window's mean, as metrics_channel. */
    if (used < total) {
        f |= SDR_RD_DC_MASKED;
        if (used) sum = sum * total / used;
    }
    if (flags) *flags = f;
    if (!sum) return SDR_LEVEL_NONE;
    int32_t q = floor_div256(metrics_code_q8(sum) + scale_q8 + 128);
    if (q > INT16_MAX) q = INT16_MAX;
    if (q <= SDR_LEVEL_NONE) q = SDR_LEVEL_NONE + 1;
    return (int16_t)q;
}
