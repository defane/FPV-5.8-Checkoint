/* GPL-3.0-or-later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Ported to FPV 5.8 Checkpoint from widscan58 by CapnBry (https://github.com/CapnBry/widscan58),
 * commit a9fff97, firmware/src/scan/planner.c, with the c5-race LO-planning constants of
 * firmware/src/scan/profile.c.
 * Changes for FPV 5.8 Checkpoint: profile_t replaced by sdr_plan_cfg_t, no ISM plan and no enable
 * mask; the integer bin windows (sdr_plan_band, sdr_plan_bands) added.
 * This file keeps its GPL-3.0-or-later notice; the repository as a whole is GPL-3.0-only. */
#include <stdlib.h>
#include <string.h>
#include "sdr_plan.h"

sdr_plan_cfg_t sdr_plan_cfg_default(void) {
    /* c5-race: one 80 MS/s capture covers +-40 MHz, so a pair 37 MHz apart shares a LO; a lone
     * channel sits 8 MHz above its LO; the C5 tunes whole MHz. */
    return (sdr_plan_cfg_t){.hop_max_span_khz = 40000, .single_lo_offset_khz = -8000, .lo_step_khz = 1000};
}

static uint32_t round_step(int64_t khz, uint32_t step) {
    if (step <= 1) return (uint32_t)khz;
    return (uint32_t)(((khz + step / 2) / step) * step);
}

/* LO for channels khz[0..n-1] (ascending). Candidates: the single-mode offset
 * below the lowest and above the highest channel, the middle of every gap,
 * and both ends of the range that keeps every channel within reach.
 * Best: the largest distance from the LO (DC) to the nearest channel, up to
 * the single offset (more buys nothing), then the smallest distance to the
 * farthest one. Candidates must keep every
 * channel within reach = max(span limit / 2, single offset) of the LO, so a
 * group never reaches further into the filter skirt than the widest allowed
 * pair or a single LO does. Three channels evenly over the full span leave
 * no gap within reach: the range ends put DC just off the middle channel
 * rather than on it. */
static uint32_t place_lo(const sdr_plan_cfg_t *p, const uint32_t *khz, unsigned n) {
    int64_t d = llabs((int64_t)p->single_lo_offset_khz);
    int64_t reach = (int64_t)p->hop_max_span_khz / 2 > d ? (int64_t)p->hop_max_span_khz / 2 : d;
    reach += p->lo_step_khz;
    int64_t cand[SDR_RX_CHANNELS + 3];
    unsigned nc = 0;
    /* Single-mode side first: a lone channel keeps its usual LO. */
    if (p->single_lo_offset_khz <= 0) {
        cand[nc++] = (int64_t)khz[0] - d;
        cand[nc++] = (int64_t)khz[n - 1] + d;
    } else {
        cand[nc++] = (int64_t)khz[n - 1] + d;
        cand[nc++] = (int64_t)khz[0] - d;
    }
    for (unsigned j = 1; j < n; j++) cand[nc++] = ((int64_t)khz[j - 1] + khz[j]) / 2;
    /* Range ends, rounded inward to the LO step. */
    int64_t step = p->lo_step_khz > 1 ? p->lo_step_khz : 1;
    int64_t lo_end = (int64_t)khz[n - 1] - reach, hi_end = (int64_t)khz[0] + reach;
    cand[nc++] = (lo_end + step - 1) / step * step;
    cand[nc++] = hi_end / step * step;
    uint32_t best = round_step(((int64_t)khz[0] + khz[n - 1]) / 2, p->lo_step_khz);
    int64_t best_near = -1, best_far = 0;
    for (unsigned c = 0; c < nc; c++) {
        uint32_t lo = round_step(cand[c], p->lo_step_khz);
        int64_t near = INT64_MAX, far = 0;
        for (unsigned j = 0; j < n; j++) {
            int64_t off = llabs((int64_t)khz[j] - lo);
            if (off < near) near = off;
            if (off > far) far = off;
        }
        if (far > reach) continue;
        if (near > d) near = d;
        if (near > best_near || (near == best_near && far < best_far)) {
            best = lo;
            best_near = near;
            best_far = far;
        }
    }
    return best;
}

void sdr_plan_build(const uint32_t *ch_khz, unsigned n, sdr_plan_mode_t mode,
                    const sdr_plan_cfg_t *cfg, sdr_plan_t *out) {
    uint8_t order[SDR_RX_CHANNELS];
    memset(out, 0, sizeof(*out));
    if (n > SDR_RX_CHANNELS) n = SDR_RX_CHANNELS;
    for (unsigned j = 0; j < n; j++) order[j] = (uint8_t)j;
    /* Insertion sort by frequency; tables are tiny. */
    for (unsigned j = 1; j < n; j++) {
        uint8_t v = order[j];
        unsigned k = j;
        while (k && ch_khz[order[k - 1]] > ch_khz[v]) { order[k] = order[k - 1]; k--; }
        order[k] = v;
    }
    /* Greedy from the lowest channel: optimal for covering points with
     * fixed-width windows. */
    for (unsigned j = 0; j < n;) {
        sdr_plan_hop_t *h = &out->hop[out->nhops++];
        unsigned m = 1;
        if (mode == SDR_PLAN_PAIRS)
            while (j + m < n && ch_khz[order[j + m]] - ch_khz[order[j]] <= cfg->hop_max_span_khz) m++;
        uint32_t khz[SDR_RX_CHANNELS];
        for (unsigned k = 0; k < m; k++) khz[k] = ch_khz[order[j + k]];
        h->lo_khz = m == 1 && mode == SDR_PLAN_SINGLE
                        ? round_step((int64_t)khz[0] + cfg->single_lo_offset_khz, cfg->lo_step_khz)
                        : place_lo(cfg, khz, m);
        h->nch = (uint8_t)m;
        for (unsigned k = 0; k < m; k++) {
            h->ch[k] = order[j + k];
            h->offset_hz[k] = ((int32_t)khz[k] - (int32_t)h->lo_khz) * 1000;
        }
        j += m;
    }
}

/* floor(a / b) and ceil(a / b) for b > 0, a of either sign. */
static int64_t div_floor(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int64_t div_ceil(int64_t a, int64_t b) { return a >= 0 ? (a + b - 1) / b : -(-a / b); }

sdr_band_t sdr_plan_band(unsigned nfft, uint32_t rate_hz, int32_t offset_hz, uint32_t half_hz) {
    if (!nfft || !rate_hz) return (sdr_band_t){0, -1};
    const int64_t center = nfft / 2, last = (int64_t)nfft - 1;
    /* (off -+ half) / bin with bin = rate_hz / nfft, exact in integers. */
    int64_t r0 = center + div_ceil(((int64_t)offset_hz - half_hz) * nfft, rate_hz);
    int64_t r1 = center + div_floor(((int64_t)offset_hz + half_hz) * nfft, rate_hz);
    r0 = r0 < 0 ? 0 : r0 > last ? last : r0;
    r1 = r1 < 0 ? 0 : r1 > last ? last : r1;
    return (sdr_band_t){(int16_t)r0, (int16_t)r1};
}

void sdr_plan_bands(sdr_plan_t *p, unsigned nfft, uint32_t rate_hz, uint32_t half_hz) {
    for (unsigned h = 0; h < p->nhops; h++)
        for (unsigned j = 0; j < p->hop[h].nch; j++)
            p->hop[h].band[j] = sdr_plan_band(nfft, rate_hz, p->hop[h].offset_hz[j], half_hz);
}
