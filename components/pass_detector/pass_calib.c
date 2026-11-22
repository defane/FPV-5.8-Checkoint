/* Passage calibration: see pass_calib.h. */
#include "pass_calib.h"
#include <math.h>
#include <string.h>

void pc_hist_clear(pc_hist_t *h) { memset(h, 0, sizeof(*h)); }

void pc_hist_add(pc_hist_t *h, unsigned channel, float rise_db)
{
    if (channel >= PC_CHANNELS || rise_db != rise_db) return;   /* NaN: no sample */
    int i = (int)floorf((rise_db - PC_BASE_DB) / PC_BIN_DB);
    if (i < 0) i = 0;
    if (i >= (int)PC_BINS) i = (int)PC_BINS - 1;
    if (h->bin[channel][i] < UINT16_MAX) h->bin[channel][i]++;
}

void pc_hist_capture(pc_hist_t *h, bool saturated)
{
    if (h->caps < UINT32_MAX) h->caps++;
    if (saturated && h->sat_caps < UINT32_MAX) h->sat_caps++;
}

static int8_t q2(float db)   /* dB to half dB, rounded, in int8 */
{
    const long v = lroundf(db * 2.0f);
    return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
}

static float bin_db(unsigned i) { return PC_BASE_DB + ((float)i + 0.5f) * PC_BIN_DB; }

/* The level under which `pct` % of the samples stand (the center of the bin that crosses it). */
static float percentile(const uint16_t *bin, uint32_t total, unsigned pct)
{
    if (!total) return 0.0f;
    const uint32_t want = (total * pct + 99u) / 100u;   /* the sample that reaches it, rounded up */
    uint32_t acc = 0;
    for (unsigned i = 0; i < PC_BINS; i++) {
        acc += bin[i];
        if (acc >= want) return bin_db(i);
    }
    return bin_db(PC_BINS - 1u);
}

void pc_stats(const pc_hist_t *h, unsigned channel, pc_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (channel >= PC_CHANNELS) return;
    const uint16_t *b = h->bin[channel];
    uint32_t total = 0;
    int top = -1;
    for (unsigned i = 0; i < PC_BINS; i++) {
        total += b[i];
        if (b[i]) top = (int)i;
    }
    out->channel = (uint8_t)channel;
    out->samples = total > UINT16_MAX ? UINT16_MAX : (uint16_t)total;
    out->sat_pct = h->caps ? (uint8_t)((100ull * h->sat_caps + h->caps / 2u) / h->caps) : 0;
    if (!total) return;
    out->p10 = q2(percentile(b, total, 10));
    out->p50 = q2(percentile(b, total, 50));
    out->p90 = q2(percentile(b, total, 90));
    out->p95 = q2(percentile(b, total, 95));
    out->max = q2(bin_db((unsigned)top));
}

unsigned pc_strongest(const pc_hist_t *h)
{
    unsigned best = 0;
    int8_t best95 = -128;
    for (unsigned ch = 0; ch < PC_CHANNELS; ch++) {
        uint32_t total = 0;
        for (unsigned i = 0; i < PC_BINS; i++) total += h->bin[ch][i];
        if (!total) continue;
        const int8_t p95 = q2(percentile(h->bin[ch], total, 95));
        if (p95 > best95) { best95 = p95; best = ch; }
    }
    return best;
}

static float half(float x) { return roundf(x * 2.0f) / 2.0f; }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static int16_t x10(float db) { return (int16_t)lroundf(db * 10.0f); }

void pc_solve(const pc_stats_t *gate, const pc_stats_t *away, const pc_stats_t *away_top, pc_result_t *out)
{
    memset(out, 0, sizeof(*out));
    /* What the C5 starts with (sd_config_default): kept when nothing could be measured. */
    out->active_db_x10 = 80; out->hysteresis_db_x10 = 30; out->drop_db_x10 = 30; out->min_peak_db_x10 = 100;
    const float g10 = gate->p10 / 2.0f, g90 = gate->p90 / 2.0f, g95 = gate->p95 / 2.0f;
    const float b50 = away->p50 / 2.0f, b95 = away->p95 / 2.0f;

    if (gate->samples == 0 || g95 < PC_SEEN_DB) { out->flags |= PC_NO_DRONE; return; }
    if (gate->sat_pct >= PC_CLIP_PCT) out->flags |= PC_CLIPPING;
    if (away->samples == 0 || b95 < PC_AWAY_SEEN_DB) out->flags |= PC_AWAY_WEAK;
    if (away_top && away_top->channel != gate->channel && away_top->p95 >= away->p95 + 12) out->flags |= PC_OTHER_CHANNEL;

    /* A pass must reach a fixed margin over what the drone reads away from the gate; when the two
     * levels are too close for that (the hover's low end under the margin), halfway between them,
     * flagged. active_db sits 2 dB under it (pass_detector wants min_peak >= active). */
    float min_peak = b95 + PC_MARGIN_DB;
    if (g10 - 2.0f < min_peak) {
        out->flags |= PC_LOW_SEPARATION;
        min_peak = (b95 + g10) / 2.0f;
    }
    if (g10 - b95 < PC_SEPARATION_DB) out->flags |= PC_LOW_SEPARATION;
    min_peak = half(clampf(min_peak, PC_MIN_PEAK_LO_DB, PC_MIN_PEAK_HI_DB));
    const float active = half(clampf(min_peak - 2.0f, PC_MIN_PEAK_LO_DB - 2.0f, PC_MIN_PEAK_HI_DB - 2.0f));
    /* The ripple the drone shows away from the gate sets the release; its spread in the passage the drop. */
    const float hyst = half(clampf(b95 - b50, 2.0f, 6.0f));
    const float drop = half(clampf((g90 - g10) / 2.0f, 3.0f, 6.0f));

    out->min_peak_db_x10 = x10(min_peak);
    out->active_db_x10 = x10(active);
    out->hysteresis_db_x10 = x10(hyst);
    out->drop_db_x10 = x10(drop);
    out->valid = true;
    out->gain_steps = (out->flags & PC_CLIPPING) ? -1 : (out->flags & PC_AWAY_WEAK) ? 1 : 0;
}
