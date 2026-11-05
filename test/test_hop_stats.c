/* Host test for the hop bench statistics. Build:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/checkpoint test_hop_stats.c -o /tmp/t && /tmp/t
 */
#include "hop_stats.h"
#include <stdio.h>

static int fails;
#define CHECK(name, cond) do { \
    if (cond) { printf("ok: %s\n", (name)); } \
    else { printf("FAIL: %s\n", (name)); fails++; } } while (0)

#define N 12u

int main(void)
{
    uint32_t v[] = {30, 10, 50, 20, 40};
    hs_spread_t s = hs_spread(v, 5);
    CHECK("spread min/med/max", s.min == 10 && s.med == 30 && s.max == 50);
    CHECK("spread sorted in place", v[0] == 10 && v[4] == 50);
    uint32_t odd[] = {7};
    s = hs_spread(odd, 1);
    CHECK("spread of one", s.min == 7 && s.med == 7 && s.max == 7);
    s = hs_spread(odd, 0);
    CHECK("spread of none", s.min == 0 && s.med == 0 && s.max == 0);

    const uint32_t table[] = {3, 9, 2, 40, 7};
    CHECK("strongest channel", hs_strongest(table, 5) == 3);
    CHECK("strongest of one", hs_strongest(table, 1) == 0);

    CHECK("near: equal", hs_near(1000, 1000));
    CHECK("near: +3 dB edge", hs_near(2000, 1000) && !hs_near(2001, 1000));
    CHECK("near: -3 dB edge", hs_near(500, 1000) && !hs_near(499, 1000));
    CHECK("median of 3", hs_median3(1, 2, 3) == 2 && hs_median3(3, 1, 2) == 2 && hs_median3(2, 3, 1) == 2 &&
                         hs_median3(5, 5, 1) == 5 && hs_median3(1, 5, 5) == 5 && hs_median3(4, 4, 4) == 4);
    CHECK("near: no overflow on big values", hs_near(4000000000u, 4000000000u));

    uint32_t t[N], p[N], level = 0;
    for (unsigned i = 0; i < N; i++) t[i] = (i + 1u) * 100u;

    for (unsigned i = 0; i < N; i++) p[i] = 1000;
    CHECK("flat trace settles at once", hs_settle_us(t, p, N, 4, &level) == 100 && level == 1000);

    for (unsigned i = 0; i < N; i++) p[i] = i < 5u ? 20000 : 1000;   /* the old channel's carrier */
    CHECK("falling edge: first point in band", hs_settle_us(t, p, N, 4, &level) == 600 && level == 1000);

    for (unsigned i = 0; i < N; i++) p[i] = i < 3u ? 10 : 8000;      /* the new channel's carrier */
    CHECK("rising edge", hs_settle_us(t, p, N, 4, &level) == 400 && level == 8000);

    for (unsigned i = 0; i < N; i++) p[i] = i == 6u ? 9000 : 1000;   /* one odd window */
    CHECK("a lone window does not count", hs_settle_us(t, p, N, 4, &level) == 100);

    for (unsigned i = 0; i < N; i++) p[i] = i == 6u || i == 7u ? 9000 : 1000;
    CHECK("two windows do move settling past them", hs_settle_us(t, p, N, 4, &level) == 900);

    for (unsigned i = 0; i < N; i++) p[i] = i % 3u ? 1000 : 1700;    /* a live VTX: +-2 dB */
    CHECK("a modulated carrier settles at once", hs_settle_us(t, p, N, 4, &level) == 100);

    for (unsigned i = 0; i < N; i++) p[i] = 100u << i;               /* never holds still */
    CHECK("a ramp never settles", hs_settle_us(t, p, N, 4, &level) == HS_UNSETTLED);

    for (unsigned i = 0; i < N; i++) p[i] = i % 2u ? 100 : 10000;
    CHECK("a bouncing tail does not settle", hs_settle_us(t, p, N, 4, &level) == HS_UNSETTLED);

    CHECK("too few points", hs_settle_us(t, p, 3, 4, &level) == HS_UNSETTLED);
    CHECK("no points", hs_settle_us(t, p, 0, 4, &level) == HS_UNSETTLED && level == 0);
    CHECK("tail over the maximum", hs_settle_us(t, p, N, HS_TAIL_MAX + 1u, &level) == HS_UNSETTLED);
    CHECK("trace over the maximum", hs_settle_us(t, p, HS_TRACE_MAX + 1u, 4, &level) == HS_UNSETTLED);

    /* The issue's budget: 8 channels. */
    CHECK("1 kHz needs a 125 us slot", hs_rate_x10(0, 25, 100, 8) == 10000);
    CHECK("1 ms hop + 102 us window", hs_rate_x10(1000, 0, 102, 8) == 1134);
    CHECK("130 ms per channel is about 1 Hz", hs_rate_x10(130000, 0, 0, 8) == 9);
    CHECK("zero slot", hs_rate_x10(0, 0, 0, 8) == 0);
    CHECK("no overflow on a huge slot", hs_rate_x10(UINT32_MAX, UINT32_MAX, 0, 8) == 0);

    if (fails) { printf("%d test(s) FAILED\n", fails); return 1; }
    printf("all hop_stats tests passed\n");
    return 0;
}
