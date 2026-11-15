/* Host test for the engine bench statistics. Build:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/checkpoint test_hop_stats.c -lm -o /tmp/t && /tmp/t
 */
#include "hop_stats.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(name, cond) do { \
    if (cond) { printf("ok: %s\n", (name)); } \
    else { printf("FAIL: %s\n", (name)); fails++; } } while (0)

#define N 12u

static int near_f(float a, float b, float tol) { return fabsf(a - b) <= tol; }

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
    CHECK("median of 3", hs_median3(1, 2, 3) == 2 && hs_median3(3, 1, 2) == 2 && hs_median3(2, 3, 1) == 2 &&
                         hs_median3(5, 5, 1) == 5 && hs_median3(1, 5, 5) == 5 && hs_median3(4, 4, 4) == 4);

    CHECK("ring not full", hs_ring_kept(3, 10) == 3 && hs_ring_oldest(3, 10) == 0);
    CHECK("ring exactly full", hs_ring_kept(10, 10) == 10 && hs_ring_oldest(10, 10) == 0);
    CHECK("ring wrapped", hs_ring_kept(13, 10) == 10 && hs_ring_oldest(13, 10) == 3);
    CHECK("empty ring", hs_ring_kept(0, 10) == 0 && hs_ring_oldest(0, 10) == 0);

    /* Histogram: the same min/median/max as hs_spread on the values it was given. */
    uint32_t bins[64], vals[200];
    hs_hist_t h;
    hs_hist_init(&h, bins, 64);
    s = hs_hist_spread(&h);
    CHECK("histogram: empty", h.count == 0 && s.min == 0 && s.med == 0 && s.max == 0);
    unsigned x = 12345u;
    for (unsigned i = 0; i < 200; i++) {
        x = x * 1103515245u + 12345u;
        vals[i] = 5u + (x >> 16) % 50u;
        hs_hist_add(&h, vals[i]);
    }
    const hs_spread_t hs = hs_hist_spread(&h);
    s = hs_spread(vals, 200);
    CHECK("histogram = sorted spread", hs.min == s.min && hs.med == s.med && hs.max == s.max && h.count == 200);
    hs_hist_init(&h, bins, 64);
    for (unsigned i = 0; i < 101; i++) hs_hist_add(&h, 33);
    s = hs_hist_spread(&h);
    CHECK("histogram: all equal", s.min == 33 && s.med == 33 && s.max == 33);
    hs_hist_init(&h, bins, 64);
    hs_hist_add(&h, 10);
    hs_hist_add(&h, 20);
    s = hs_hist_spread(&h);
    CHECK("histogram: even count takes the upper middle, as hs_spread", s.med == 20);
    hs_hist_init(&h, bins, 64);
    hs_hist_add(&h, 2);
    hs_hist_add(&h, 41000);
    hs_hist_add(&h, 90000);
    s = hs_hist_spread(&h);
    CHECK("histogram: over-range values keep an exact max, a median in the last bin", s.min == 2 && s.med == 63 &&
                                                                                  s.max == 90000);
    CHECK("histogram: over-range values count in the last bin", bins[63] == 2 && bins[2] == 1 && h.count == 3);
    hs_hist_init(&h, bins, 64);
    hs_hist_add(&h, 0);
    s = hs_hist_spread(&h);
    CHECK("histogram: zero", s.min == 0 && s.med == 0 && s.max == 0 && h.count == 1);

    CHECK("near qdb", hs_near_qdb(-100, -96, 4) && !hs_near_qdb(-100, -95, 4) && hs_near_qdb(5, 5, 0));
    CHECK("near qdb: no overflow at the extremes", !hs_near_qdb(INT32_MIN, INT32_MAX, 4) &&
                                                   hs_near_qdb(INT32_MIN, INT32_MIN, 0));

    int16_t after[N], ctrl[N];
    for (unsigned i = 0; i < N; i++) { after[i] = -200; ctrl[i] = -200; }
    CHECK("settled from: flat settles at once", hs_settled_from(after, ctrl, N, 4) == 0);
    for (unsigned i = 0; i < N; i++) after[i] = i < 5u ? -120 : -202;          /* the strong LO's residue */
    CHECK("settled from: residue then within 1 dB", hs_settled_from(after, ctrl, N, 4) == 5);
    after[8] = -190;                                                            /* a late excursion */
    CHECK("settled from: a late excursion moves it past", hs_settled_from(after, ctrl, N, 4) == 9);
    for (unsigned i = 0; i < N; i++) { after[i] = -150; ctrl[i] = (int16_t)(-200 + (int)i); }
    CHECK("settled from: never settles", hs_settled_from(after, ctrl, N, 4) == N);
    for (unsigned i = 0; i < N; i++) { ctrl[i] = (int16_t)(-200 + 10 * (int)i); after[i] = (int16_t)(ctrl[i] + 3); }
    CHECK("settled from: pointwise against a moving control", hs_settled_from(after, ctrl, N, 4) == 0);
    CHECK("settled from: no points", hs_settled_from(after, ctrl, 0, 4) == 0);

    int32_t mean = 1;
    uint32_t sd = 1;
    const int16_t flat[] = {-240, -240, -240, -240};
    hs_mean_sd_qdb(flat, 4, &mean, &sd);
    CHECK("mean/sd: all equal", mean == -240 && sd == 0);
    hs_mean_sd_qdb(flat, 0, &mean, &sd);
    CHECK("mean/sd: none", mean == 0 && sd == 0);
    hs_mean_sd_qdb(flat, 1, &mean, &sd);
    CHECK("mean/sd: one", mean == -240 && sd == 0);
    const int16_t two[] = {-244, -236};                       /* -61 and -59 dBFS */
    hs_mean_sd_qdb(two, 2, &mean, &sd);
    CHECK("mean/sd: sample sd in mdB", mean == -240 && sd == 1414);
    const int16_t half[] = {-241, -240};                      /* mean -240.5 rounds away from zero */
    hs_mean_sd_qdb(half, 2, &mean, &sd);
    CHECK("mean/sd: negative mean rounds to the nearest", mean == -241);
    int16_t noise[1000];
    unsigned y = 1u;
    for (unsigned i = 0; i < 1000; i++) {
        y ^= y << 13; y ^= y >> 17; y ^= y << 5;
        noise[i] = (int16_t)(-240 + (int)(y % 9u) - 4);      /* uniform -4..+4 qdb: sd 2.58 qdb */
    }
    hs_mean_sd_qdb(noise, 1000, &mean, &sd);
    CHECK("mean/sd: uniform noise", abs(mean + 240) <= 1 && sd > 600 && sd < 690);

    int32_t lv[] = {-96, -240, 12, -240, -300};
    CHECK("strongest i32 with negatives", hs_strongest_i32(lv, 5) == 2);
    CHECK("weakest i32 with negatives", hs_weakest_i32(lv, 5) == 4);
    CHECK("weakest i32: first of equals", hs_weakest_i32(lv, 4) == 1);
    CHECK("strongest/weakest i32 of none", hs_strongest_i32(lv, 0) == 0 && hs_weakest_i32(lv, 0) == 0);
    hs_spread_i32_t si = hs_spread_i32(lv, 5);
    CHECK("spread i32 sorts, negatives", si.min == -300 && si.median == -240 && si.max == 12 && lv[0] == -300 &&
                                         lv[4] == 12);
    si = hs_spread_i32(lv, 0);
    CHECK("spread i32 of none", si.min == 0 && si.median == 0 && si.max == 0);
    int32_t even[] = {-10, -20};
    si = hs_spread_i32(even, 2);
    CHECK("spread i32 even count takes the upper middle, as hs_spread", si.median == -10);
    int32_t eq[] = {-7, -7, -7};
    si = hs_spread_i32(eq, 3);
    CHECK("spread i32 all equal", si.min == -7 && si.median == -7 && si.max == -7);
    int32_t big[] = {INT32_MAX, INT32_MIN, 0};
    si = hs_spread_i32(big, 3);
    CHECK("spread i32 extremes", si.min == INT32_MIN && si.median == 0 && si.max == INT32_MAX);

    CHECK("excess: twice the floor is the floor", near_f(hs_excess_db(-60.0f + 3.0103f, -60.0f), -60.0f, 0.01f));
    CHECK("excess: 0.5 dB over the floor is 9.1 dB under it",
          near_f(hs_excess_db(-59.5f, -60.0f), -60.0f - 9.14f, 0.02f));
    CHECK("excess: 20 dB over", near_f(hs_excess_db(-40.0f, -60.0f), -40.0436f, 0.001f));
    CHECK("excess: equal is -200", hs_excess_db(-60.0f, -60.0f) == -200.0f);
    CHECK("excess: under the floor is -200", hs_excess_db(-61.0f, -60.0f) == -200.0f);
    CHECK("excess: far over is the reading", hs_excess_db(40.0f, -60.0f) == 40.0f);
    CHECK("excess: positive levels", near_f(hs_excess_db(13.0103f, 10.0f), 10.0f, 0.01f));
    CHECK("excess: a hair over stays finite", hs_excess_db(-60.0f + 1e-6f, -60.0f) >= -200.0f);

    if (fails) { printf("%d test(s) FAILED\n", fails); return 1; }
    printf("all hop_stats tests passed\n");
    return 0;
}
