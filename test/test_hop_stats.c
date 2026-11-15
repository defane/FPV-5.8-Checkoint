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

    /* K x's floor check (HS_X_FLOOR_SPREAD_QDB = 24, 6 dB), on the board's medians of 2026-11-15 at
     * gain 40, R1..R8 in qdb. */
    {
        const int32_t none = INT16_MIN, margin = HS_X_FLOOR_SPREAD_QDB;
        CHECK("K x margins: a floor allows 6 dB over the median, a signal is 10 dB over its floor",
              HS_X_FLOOR_SPREAD_QDB == 24 && HS_X_SIGNAL_QDB == 40);
        int32_t tmp[8], over = -1;
        const int32_t off[8] = {-239, -237, -239, -238, -237, -236, -237, -234};   /* K n, VTX off */
        int top = hs_over_median_i32(off, 8, none, tmp, &over);
        CHECK("over median: VTX off, R8 0.75 dB over the median, a floor", top == 7 && over == 3 && over <= margin);
        CHECK("over median: v is left as it was", off[0] == -239 && off[7] == -234);
        const int32_t r6_on[8] = {-239, -236, -238, -236, -221, -8, -238, -233};  /* a floor run, R6 on */
        top = hs_over_median_i32(r6_on, 8, none, tmp, &over);
        CHECK("over median: R6 on, 57 dB over the median (its partner R5 at +3.75 dB), refused",
              top == 5 && over == 228 && over > margin);
        const int32_t r6_far[8] = {-239, -238, -239, -237, -236, -41, -239, -234};
        top = hs_over_median_i32(r6_far, 8, none, tmp, &over);
        CHECK("over median: the weakest R6 floor run, 49 dB over, refused", top == 5 && over == 196 && over > margin);
        const int32_t three[8] = {-238, -100, -238, -120, -238, -140, -238, -238};  /* 3 channels raised */
        top = hs_over_median_i32(three, 8, none, tmp, &over);
        CHECK("over median: 3 raised channels of 8 leave the median on the floor", top == 1 && over == 138);
        const int32_t gaps[8] = {none, -239, none, -230, none, -238, none, -237};  /* LOs without readings */
        top = hs_over_median_i32(gaps, 8, none, tmp, &over);
        CHECK("over median: missing channels left out", top == 3 && over == 7);
        const int32_t all_none[2] = {none, none};
        top = hs_over_median_i32(all_none, 2, none, tmp, &over);
        CHECK("over median: no value", top == -1 && over == 0);
        top = hs_over_median_i32(off, 1, none, tmp, &over);
        CHECK("over median: one value is its own median", top == 0 && over == 0);
        /* Run 2 with the VTX too weak to count as a signal (under 10 dB over its floor): R1 8 dB over
         * its stored floor is 7.5 dB over the median. With that floor stored, K x asks to bring the VTX
         * closer; refusing as a floor run would send the operator back to store the same floor. */
        const int32_t r1_weak[8] = {-207, -237, -239, -238, -237, -236, -237, -234};
        top = hs_over_median_i32(r1_weak, 8, none, tmp, &over);
        CHECK("K x: a weak run 2 is weak with a floor stored, a refused floor run without",
              top == 0 && over == 30 && hs_x_run(over, margin, true) == HS_X_WEAK &&
                  hs_x_run(over, margin, false) == HS_X_REFUSED);
        (void)hs_over_median_i32(off, 8, none, tmp, &over);
        CHECK("K x: every VTX off is a floor run, stored floor or not",
              hs_x_run(over, margin, true) == HS_X_FLOOR && hs_x_run(over, margin, false) == HS_X_FLOOR &&
                  hs_x_run(0, margin, true) == HS_X_FLOOR);

        /* hs_x_verdict, the whole decision bench_image takes: no floor stored (NULL) or the VTX-off
         * medians stored as the floor of this gain. */
        int16_t floor16[8];
        for (unsigned i = 0; i < 8u; i++) floor16[i] = (int16_t)off[i];
        int32_t ov[8];
        hs_x_verdict_t v = hs_x_verdict(off, NULL, 8, none, ov, tmp);
        CHECK("verdict: VTX off, no floor stored: a floor run", v.run == HS_X_FLOOR && v.top == 7 && v.excess == 3 &&
                                                                    ov[0] == INT32_MIN && ov[7] == INT32_MIN);
        v = hs_x_verdict(off, floor16, 8, none, ov, tmp);
        CHECK("verdict: VTX off again over its floor: a floor run (stored again)", v.run == HS_X_FLOOR && ov[3] == 0);
        v = hs_x_verdict(r6_on, NULL, 8, none, ov, tmp);
        CHECK("verdict: R6 on, no floor stored: refused, R6 57 dB over the median",
              v.run == HS_X_REFUSED && v.top == 5 && v.excess == 228);
        v = hs_x_verdict(r6_on, floor16, 8, none, ov, tmp);
        CHECK("verdict: R6 on over a stored floor: run 2 with R6", v.run == HS_X_SIGNAL && v.top == 5 && v.excess == 228 &&
                                                                       ov[4] == 16);
        v = hs_x_verdict(r1_weak, floor16, 8, none, ov, tmp);
        CHECK("verdict: R1 8 dB over its stored floor: weak, bring it closer", v.run == HS_X_WEAK && v.top == 0 && v.excess == 32);
        v = hs_x_verdict(r1_weak, NULL, 8, none, ov, tmp);
        CHECK("verdict: the same without a floor stored: a refused floor run", v.run == HS_X_REFUSED && v.top == 0 && v.excess == 30);
        /* The margins to the code: R8 24 qdb (6 dB) over the median is a floor, 25 is not; R2 40 qdb
         * (10 dB) over its floor is a signal, 39 is weak. */
        int32_t edge[8] = {-238, -238, -238, -238, -238, -238, -238, -214};
        v = hs_x_verdict(edge, NULL, 8, none, ov, tmp);
        const bool at24 = v.run == HS_X_FLOOR && v.excess == 24;
        edge[7] = -213;
        v = hs_x_verdict(edge, NULL, 8, none, ov, tmp);
        CHECK("verdict: 6 dB over the median is a floor run, 6.25 dB is refused", at24 && v.run == HS_X_REFUSED && v.excess == 25);
        int32_t sig[8];
        for (unsigned i = 0; i < 8u; i++) sig[i] = off[i];
        sig[1] = off[1] + 40;
        v = hs_x_verdict(sig, floor16, 8, none, ov, tmp);
        const bool at40 = v.run == HS_X_SIGNAL && v.top == 1 && v.excess == 40;
        sig[1] = off[1] + 39;
        v = hs_x_verdict(sig, floor16, 8, none, ov, tmp);
        CHECK("verdict: 10 dB over its floor is run 2, 9.75 dB is weak", at40 && v.run == HS_X_WEAK && v.top == 1 && v.excess == 39);
        /* A stored floor with no channel to compare (LOs without readings then): nothing to name as
         * weak, so a raised run is refused as a floor run. */
        int16_t nofloor[8];
        for (unsigned i = 0; i < 8u; i++) nofloor[i] = (int16_t)none;
        v = hs_x_verdict(r6_on, nofloor, 8, none, ov, tmp);
        CHECK("verdict: a stored floor without one channel in it: refused", v.run == HS_X_REFUSED && v.top == 5 && ov[5] == INT32_MIN);
        const int32_t r6_gap[8] = {-239, -236, -238, -236, none, -8, -238, -233};
        v = hs_x_verdict(r6_gap, floor16, 8, none, ov, tmp);
        CHECK("verdict: a channel without a reading has no rise over its floor", v.run == HS_X_SIGNAL && ov[4] == INT32_MIN);
    }

    /* K S's saturated word (bench.md section 5: hop 0 in the top digit, `0f00` = R3+R4 clipped 15). */
    {
        const unsigned w0f00[HS_SAT_HOPS] = {0, 15, 0, 0}, w1234[HS_SAT_HOPS] = {1, 2, 3, 4}, wcap[HS_SAT_HOPS] = {16, 0, 0, 3},
                       wbig[HS_SAT_HOPS] = {255, 300, 15, 14}, wnone[HS_SAT_HOPS] = {0, 0, 0, 0};
        CHECK("sat word: the second hop clipped 15 times reads 0f00", hs_sat_pack(w0f00) == 0x0f00u);
        CHECK("sat word: hop 0 in the top nibble", hs_sat_pack(w1234) == 0x1234u);
        CHECK("sat word: a count over 15 (decim 16) reads 15, never wraps", hs_sat_pack(wcap) == 0xf003u &&
                                                                              hs_sat_pack(wbig) == 0xfffeu);
        CHECK("sat word: nothing clipped", hs_sat_pack(wnone) == 0u);
    }

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
