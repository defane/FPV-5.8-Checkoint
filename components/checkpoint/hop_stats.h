/* Engine bench statistics, pure C (host-tested in test/test_hop_stats.c), for the console benches
 * of cp_bench.c: the spread of the hop timings (unsigned microseconds), and levels in 0.25 dB
 * codes (signed qdb, as sdr_rx reports them): medians, the highest value over the median, the
 * settle point of a trace against a control, the noise of a reading, and the excess power over a
 * floor. Also the benches' decisions a test should see: what a K x run is, and K S's packing of a
 * row's saturated captures. */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t min, med, max; } hs_spread_t;

static inline void hs_sort(uint32_t *v, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        const uint32_t x = v[i];
        size_t j = i;
        for (; j && v[j - 1] > x; j--) v[j] = v[j - 1];
        v[j] = x;
    }
}

/* Sorts v in place. */
static inline hs_spread_t hs_spread(uint32_t *v, size_t n)
{
    if (!n) return (hs_spread_t){0, 0, 0};
    hs_sort(v, n);
    return (hs_spread_t){v[0], v[n / 2], v[n - 1]};
}

/* Index of the highest value of v, n > 0. */
static inline size_t hs_strongest(const uint32_t *v, size_t n)
{
    size_t best = 0;
    for (size_t i = 1; i < n; i++)
        if (v[i] > v[best]) best = i;
    return best;
}

static inline uint32_t hs_median3(uint32_t a, uint32_t b, uint32_t c)
{
    return a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b));
}

/* A ring of cap entries after count writes: how many it holds, and its oldest slot. */
static inline size_t hs_ring_kept(uint32_t count, size_t cap) { return count < cap ? count : cap; }
static inline size_t hs_ring_oldest(uint32_t count, size_t cap) { return count < cap ? 0 : count % cap; }

/* Many integer timings without keeping them (K s: thousands of hops): a count per value
 * 0..nbins-1, larger values in the last bin (a median there reads nbins - 1); min and max
 * exact. bin[] is the caller's. */
typedef struct { uint32_t *bin; uint32_t nbins, count, min, max; } hs_hist_t;

static inline void hs_hist_init(hs_hist_t *h, uint32_t *bin, uint32_t nbins)
{
    for (uint32_t i = 0; i < nbins; i++) bin[i] = 0;
    *h = (hs_hist_t){bin, nbins, 0, UINT32_MAX, 0};
}

static inline void hs_hist_add(hs_hist_t *h, uint32_t v)
{
    if (!h->nbins) return;
    h->bin[v < h->nbins ? v : h->nbins - 1u]++;
    h->count++;
    if (v < h->min) h->min = v;
    if (v > h->max) h->max = v;
}

/* As hs_spread on the same values: the median is the (count / 2)-th smallest, from 0. */
static inline hs_spread_t hs_hist_spread(const hs_hist_t *h)
{
    if (!h->count) return (hs_spread_t){0, 0, 0};
    uint32_t below = 0, m = 0;
    for (; m + 1u < h->nbins; m++) {
        below += h->bin[m];
        if (below > h->count / 2u) break;
    }
    return (hs_spread_t){h->min, m, h->max};
}

/* ---- levels: 0.25 dB codes, signed ---- */

static inline int hs_near_qdb(int32_t a, int32_t b, int32_t tol)   /* |a-b| <= tol */
{
    const int64_t d = (int64_t)a - (int64_t)b;
    return (d < 0 ? -d : d) <= (int64_t)tol;
}

/* The first index from which every later point stays within tol of ref[i] (pointwise), or n
 * when not even the last one does. */
static inline size_t hs_settled_from(const int16_t *v, const int16_t *ref, size_t n, int32_t tol)
{
    size_t first = n;
    while (first && hs_near_qdb(v[first - 1], ref[first - 1], tol)) first--;
    return first;
}

/* Mean (rounded, qdb) and sample standard deviation (millidB) of n codes; 0 and 0 for n = 0,
 * sd 0 for n = 1. Every v counts: leave missing readings out first. */
static inline void hs_mean_sd_qdb(const int16_t *v, size_t n, int32_t *mean_qdb, uint32_t *sd_mdb)
{
    *mean_qdb = 0;
    *sd_mdb = 0;
    if (!n) return;
    int64_t s = 0;
    for (size_t i = 0; i < n; i++) s += v[i];
    const double mean = (double)s / (double)n;
    *mean_qdb = (int32_t)lround(mean);
    if (n < 2) return;
    double ss = 0.0;
    for (size_t i = 0; i < n; i++) ss += ((double)v[i] - mean) * ((double)v[i] - mean);
    *sd_mdb = (uint32_t)lround(sqrt(ss / (double)(n - 1)) * 250.0);   /* 1 qdb = 250 mdB */
}

typedef struct { int32_t min, median, max; } hs_spread_i32_t;

static inline void hs_sort_i32(int32_t *v, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        const int32_t x = v[i];
        size_t j = i;
        for (; j && v[j - 1] > x; j--) v[j] = v[j - 1];
        v[j] = x;
    }
}

/* Sorts v; n = 0 gives all 0. The median is v[n / 2] of the sorted values, as hs_spread. */
static inline hs_spread_i32_t hs_spread_i32(int32_t *v, size_t n)
{
    if (!n) return (hs_spread_i32_t){0, 0, 0};
    hs_sort_i32(v, n);
    return (hs_spread_i32_t){v[0], v[n / 2], v[n - 1]};
}

/* The highest of v's n values and its excess over their median (v[n / 2] of the sorted ones, as
 * hs_spread_i32), values equal to `none` left out: its index, or -1 when none is left, and the
 * excess in *over (0 then). tmp: the caller's room for n values. K x: a floor run with a channel
 * far over the others has a VTX on. */
static inline int hs_over_median_i32(const int32_t *v, size_t n, int32_t none, int32_t *tmp, int32_t *over)
{
    size_t k = 0;
    int best = -1;
    for (size_t i = 0; i < n; i++) {
        if (v[i] == none) continue;
        tmp[k++] = v[i];
        if (best < 0 || v[i] > v[best]) best = (int)i;
    }
    *over = best < 0 ? 0 : v[best] - hs_spread_i32(tmp, k).median;
    return best;
}

/* K x, a run with no channel HS_X_SIGNAL_QDB over a stored floor: the top channel's excess over the
 * median within `spread` is a floor run. Over it a VTX is on: with a floor stored for this gain,
 * run 2 with the VTX too weak (bring it closer); without one, a floor run refused (switch it off).
 * HS_X_SIGNAL: hs_x_verdict's run 2. */
typedef enum { HS_X_FLOOR, HS_X_WEAK, HS_X_REFUSED, HS_X_SIGNAL } hs_x_run_t;
static inline hs_x_run_t hs_x_run(int32_t excess, int32_t spread, bool floor_stored)
{
    if (excess <= spread) return HS_X_FLOOR;
    return floor_stored ? HS_X_WEAK : HS_X_REFUSED;
}

/* K x's thresholds (cp_bench.c's comment has the board's numbers): a channel this far over its
 * stored floor carries a VTX (10 dB), and a floor run allows this much over the channels' median
 * (6 dB). */
#define HS_X_SIGNAL_QDB       40
#define HS_X_FLOOR_SPREAD_QDB 24

typedef struct {
    hs_x_run_t run;
    int top;          /* FLOOR, REFUSED: the channel highest over the median (-1: no reading); WEAK,
                       * SIGNAL: the one highest over its stored floor */
    int32_t excess;   /* that channel's excess over the median, or over its floor (WEAK, SIGNAL) */
} hs_x_verdict_t;

/* What a K x run is (cp_bench.c bench_image). med: the n channels' medians, `none` without a
 * reading. floor: the floor stored for this gain, NULL when none is (no floor run yet, or one at
 * another gain); `none` for a channel it lacks. over gets each channel's rise over its stored floor,
 * INT32_MIN without one; tmp is the caller's room for n values.
 * - SIGNAL: a channel stands HS_X_SIGNAL_QDB over its floor, run 2 with that VTX.
 * - Else the top channel's excess over the run's median decides (hs_x_run, HS_X_FLOOR_SPREAD_QDB):
 *   FLOOR, store it; WEAK, run 2's VTX too weak (a floor stored, a channel over it to name); REFUSED,
 *   a floor run with a VTX on (or a weak run 2 with no channel over a stored floor to name). */
static inline hs_x_verdict_t hs_x_verdict(const int32_t *med, const int16_t *floor, size_t n, int32_t none,
                                          int32_t *over, int32_t *tmp)
{
    int strong = -1;
    for (size_t i = 0; i < n; i++) {
        const bool ok = floor && med[i] != none && floor[i] != none;
        over[i] = ok ? med[i] - floor[i] : INT32_MIN;
        if (ok && (strong < 0 || over[i] > over[strong])) strong = (int)i;
    }
    if (strong >= 0 && over[strong] >= HS_X_SIGNAL_QDB) return (hs_x_verdict_t){HS_X_SIGNAL, strong, over[strong]};
    int32_t excess = 0;
    const int top = hs_over_median_i32(med, n, none, tmp, &excess);
    const hs_x_run_t run = hs_x_run(excess, HS_X_FLOOR_SPREAD_QDB, floor != NULL);
    if (run == HS_X_WEAK && strong >= 0) return (hs_x_verdict_t){HS_X_WEAK, strong, over[strong]};
    return (hs_x_verdict_t){run == HS_X_FLOOR ? HS_X_FLOOR : HS_X_REFUSED, top, excess};
}

/* K S: a recording row's saturated captures per hop (sat[0] the first hop's) as one 16-bit word,
 * 4 bits a hop, hop 0 in the top nibble, each count capped at HS_SAT_MAX: the SCAN line's last field
 * (`0f00`: the second hop clipped 15 times or more), tools/scan_analyze.py's read_sat. */
#define HS_SAT_HOPS 4u
#define HS_SAT_MAX  15u
static inline uint16_t hs_sat_pack(const unsigned sat[HS_SAT_HOPS])
{
    uint16_t w = 0;
    for (unsigned j = 0; j < HS_SAT_HOPS; j++)
        w |= (uint16_t)((sat[j] < HS_SAT_MAX ? sat[j] : HS_SAT_MAX) << (4u * (HS_SAT_HOPS - 1u - j)));
    return w;
}

static inline size_t hs_strongest_i32(const int32_t *v, size_t n)   /* index of the max, 0 if n = 0 */
{
    size_t best = 0;
    for (size_t i = 1; i < n; i++)
        if (v[i] > v[best]) best = i;
    return best;
}

static inline size_t hs_weakest_i32(const int32_t *v, size_t n)     /* index of the min, 0 if n = 0 */
{
    size_t best = 0;
    for (size_t i = 1; i < n; i++)
        if (v[i] < v[best]) best = i;
    return best;
}

/* The power a reading has beyond a floor, dB: 10 log10(10^(with/10) - 10^(floor/10)), computed
 * relative to the floor (no overflow at any level); -200 when with <= floor. */
static inline float hs_excess_db(float with_db, float floor_db)
{
    const float d = with_db - floor_db;
    if (!(d > 0.0f)) return -200.0f;
    if (d > 60.0f) return with_db;                 /* the floor is under 1e-6 dB of it */
    const float x = powf(10.0f, d / 10.0f) - 1.0f;
    if (!(x > 0.0f)) return -200.0f;
    const float r = floor_db + 10.0f * log10f(x);
    return r < -200.0f ? -200.0f : r;
}
