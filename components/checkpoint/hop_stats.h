/* Hop bench statistics, pure C (host-tested in test/test_hop_stats.c): how long a
 * retune takes, how long the IQ power needs to settle after it, and the refresh
 * per channel a scan would get out of that. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define HS_UNSETTLED  UINT32_MAX
#define HS_TAIL_MAX   16u
#define HS_TRACE_MAX  128u

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

/* Mean IQ power of a window, DC removed, x256 (4-bit I and Q as measure_window). */
static inline uint32_t hs_window_power(const uint8_t *s, size_t n)
{
    int32_t si = 0, sq = 0;
    uint32_t sp = 0;
    for (size_t k = 0; k < n; k++) {
        const int q = (int8_t)((s[k] & 0x0fu) << 4) >> 4, i = (int8_t)(s[k] & 0xf0u) >> 4;
        si += i;
        sq += q;
        sp += (uint32_t)(i * i + q * q);
    }
    const int64_t ac = (int64_t)sp * (int64_t)n - (int64_t)si * si - (int64_t)sq * sq;
    return ac <= 0 ? 0u : (uint32_t)(ac * 256 / ((int64_t)n * (int64_t)n));
}

/* Sorts v in place. */
static inline hs_spread_t hs_spread(uint32_t *v, size_t n)
{
    if (!n) return (hs_spread_t){0, 0, 0};
    hs_sort(v, n);
    return (hs_spread_t){v[0], v[n / 2], v[n - 1]};
}

/* Index of the highest value of v, n > 0: the VTX's channel in a power table. */
static inline size_t hs_strongest(const uint32_t *v, size_t n)
{
    size_t best = 0;
    for (size_t i = 1; i < n; i++)
        if (v[i] > v[best]) best = i;
    return best;
}

/* About +-3 dB: a live VTX moves more than 1 dB from one 102 us window to the next
 * (first board run, unclipped: the video modulation), so a tighter band never holds. */
static inline int hs_near(uint32_t p, uint32_t level)
{
    return (uint64_t)p <= (uint64_t)level * 2u && (uint64_t)p * 2u >= (uint64_t)level;
}

static inline uint32_t hs_median3(uint32_t a, uint32_t b, uint32_t c)
{
    return a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b));
}

/* An IQ power trace after a hop, t_us rising, n <= HS_TRACE_MAX, median-of-3
 * smoothed so a lone window does not count. The settled level is the median of the
 * last tail points (tail <= HS_TAIL_MAX), stored in *level. Returns the time of the
 * first point from which every later one stays near it, or HS_UNSETTLED when the
 * tail itself does not hold still. */
static inline uint32_t hs_settle_us(const uint32_t *t_us, const uint32_t *power, size_t n, size_t tail,
                                    uint32_t *level)
{
    uint32_t last[HS_TAIL_MAX], s[HS_TRACE_MAX];
    if (!n || !tail || tail > HS_TAIL_MAX || tail > n || n > HS_TRACE_MAX) { *level = 0; return HS_UNSETTLED; }
    for (size_t i = 0; i < n; i++)
        s[i] = i && i + 1u < n ? hs_median3(power[i - 1], power[i], power[i + 1]) : power[i];
    for (size_t i = 0; i < tail; i++) last[i] = s[n - tail + i];
    *level = hs_spread(last, tail).med;
    size_t first = n;
    while (first && hs_near(s[first - 1], *level)) first--;
    return n - first < tail ? HS_UNSETTLED : t_us[first];
}

/* A ring of cap entries after count writes: how many it holds, and its oldest slot. */
static inline size_t hs_ring_kept(uint32_t count, size_t cap) { return count < cap ? count : cap; }
static inline size_t hs_ring_oldest(uint32_t count, size_t cap) { return count < cap ? 0 : count % cap; }

/* Refresh per channel in Hz x10 for a scan of `channels`, one slot being the hop,
 * its settling and one power window. */
static inline uint32_t hs_rate_x10(uint32_t hop_us, uint32_t settle_us, uint32_t window_us, unsigned channels)
{
    const uint64_t round_us = ((uint64_t)hop_us + settle_us + window_us) * channels;
    return round_us ? (uint32_t)(10000000u / round_us) : 0u;
}
