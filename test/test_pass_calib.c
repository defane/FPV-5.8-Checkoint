/* Host test of components/pass_detector/pass_calib.c: the histogram statistics and the passage
 * calibration's thresholds, on synthetic measurements.
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include test_pass_calib.c \
 *      ../components/pass_detector/pass_calib.c ../components/pass_detector/pass_detector.c -lm -o /tmp/t && /tmp/t */
#include "pass_calib.h"
#include "pass_detector.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(c, msg) do { if (c) { g_pass++; printf("ok   %s\n", msg); } else { g_fail++; printf("FAIL %s\n", msg); } } while (0)

static uint32_t s_rng = 12345;
static float urand(void) { s_rng = s_rng * 1664525u + 1013904223u; return (float)(s_rng >> 8) / 16777216.0f; }
static float gauss(float mean, float sd) {   /* sum of 12 uniforms */
    float s = 0;
    for (int i = 0; i < 12; i++) s += urand();
    return mean + sd * (s - 6.0f);
}

/* A measurement: `n` rotations, channel `drone` reads around mean/sd, the others sit at the rest; one
 * capture in `sat_every` clips (0: never). */
static void measure(pc_hist_t *h, unsigned drone, float mean, float sd, int n, int sat_every) {
    pc_hist_clear(h);
    for (int i = 0; i < n; i++) {
        for (unsigned ch = 0; ch < PC_CHANNELS; ch++)
            pc_hist_add(h, ch, ch == drone ? gauss(mean, sd) : gauss(0.0f, 0.4f));
        pc_hist_capture(h, sat_every && i % sat_every == 0);
    }
}

static void near(float got_x2, float want, float tol, const char *msg) {
    char m[160];
    snprintf(m, sizeof m, "%s (got %.1f, want %.1f +-%.1f)", msg, got_x2 / 2.0f, want, tol);
    CHECK(fabsf(got_x2 / 2.0f - want) <= tol, m);
}

int main(void)
{
    pc_hist_t h;
    pc_stats_t st;

    /* Percentiles of a known spread. */
    measure(&h, 3, 20.0f, 2.0f, 2500, 0);
    pc_stats(&h, 3, &st);
    CHECK(st.channel == 3 && st.samples == 2500, "the stats carry the channel and its sample count");
    near(st.p50, 20.0f, 0.6f, "p50 of N(20, 2)");
    near(st.p10, 20.0f - 1.28f * 2.0f, 0.8f, "p10 of N(20, 2)");
    near(st.p90, 20.0f + 1.28f * 2.0f, 0.8f, "p90 of N(20, 2)");
    near(st.p95, 20.0f + 1.645f * 2.0f, 0.8f, "p95 of N(20, 2)");
    CHECK(st.max >= st.p95 && st.p95 >= st.p90 && st.p90 >= st.p50 && st.p50 >= st.p10, "the percentiles are ordered");
    CHECK(pc_strongest(&h) == 3, "the strongest channel is the drone's");

    /* The quiet channels read around 0. */
    pc_stats(&h, 5, &st);
    near(st.p50, 0.0f, 0.6f, "a quiet channel reads about 0");

    /* Range ends: samples outside the bins count in the end bins, a NaN or a bad channel counts nowhere. */
    pc_hist_clear(&h);
    pc_hist_add(&h, 0, 500.0f); pc_hist_add(&h, 0, -500.0f); pc_hist_add(&h, 0, NAN); pc_hist_add(&h, 9, 3.0f);
    pc_stats(&h, 0, &st);
    CHECK(st.samples == 2 && st.max == 112 && st.p10 <= -15, "out-of-range rises land in the end bins; NaN and a bad channel are ignored");
    pc_stats(&h, 1, &st);
    CHECK(st.samples == 0 && st.p50 == 0 && st.max == 0, "an empty channel reads zero stats");

    /* Saturation. */
    measure(&h, 2, 15.0f, 1.0f, 1000, 4);
    pc_stats(&h, 2, &st);
    CHECK(st.sat_pct == 25, "a capture in 4 clipping reads 25 %");

    /* The strongest channel on a tie goes to the lowest. */
    pc_hist_clear(&h);
    for (int i = 0; i < 50; i++) { pc_hist_add(&h, 6, 10.0f); pc_hist_add(&h, 2, 10.0f); }
    CHECK(pc_strongest(&h) == 2, "a tie goes to the lowest channel");

    /* ---- the solver ---- */
    pc_hist_t hg, ha;
    pc_stats_t gate, away, top;
    pc_result_t r;

    /* A clean site: 20 dB in the passage, 8 dB two metres away. */
    measure(&hg, 3, 20.0f, 2.0f, 2500, 0);
    measure(&ha, 3, 8.0f, 1.5f, 2500, 0);
    pc_stats(&hg, 3, &gate);
    pc_stats(&ha, 3, &away);
    pc_stats(&ha, pc_strongest(&ha), &top);
    pc_solve(&gate, &away, &top, &r);
    CHECK(r.valid && r.flags == 0 && r.gain_steps == 0, "a clean site: thresholds proposed, no warning");
    near(r.min_peak_db_x10 / 5.0f, 8.0f + 1.645f * 1.5f + PC_MARGIN_DB, 1.2f, "min_peak stands the margin over the away p95");
    CHECK(r.min_peak_db_x10 < 200 && r.min_peak_db_x10 > 100, "min_peak is between the two levels");
    CHECK(r.active_db_x10 == r.min_peak_db_x10 - 20, "active_db is 2 dB under min_peak");
    CHECK(r.hysteresis_db_x10 >= 20 && r.hysteresis_db_x10 <= 60 && r.drop_db_x10 >= 30 && r.drop_db_x10 <= 60,
          "hysteresis and drop stay in their ranges");
    {
        pd_config_t c = pd_config_default();
        c.active_db = r.active_db_x10 / 10.0f; c.min_peak_db = r.min_peak_db_x10 / 10.0f;
        c.hysteresis_db = r.hysteresis_db_x10 / 10.0f; c.drop_db = r.drop_db_x10 / 10.0f;
        CHECK(pd_config_valid(&c), "the proposed thresholds are a valid pass_detector config");
    }

    /* The margin holds for any site: a lower away level lowers min_peak (the floor stays 8 dB). */
    measure(&ha, 3, 3.0f, 1.0f, 2500, 0);
    pc_stats(&ha, 3, &away);
    pc_stats(&ha, pc_strongest(&ha), &top);
    pc_solve(&gate, &away, &top, &r);
    CHECK(r.valid && r.min_peak_db_x10 == 80 && r.active_db_x10 == 60, "a quiet far side: min_peak at its 8 dB floor, active 6");

    /* No drone in the passage. */
    measure(&hg, 3, 1.0f, 0.5f, 2500, 0);
    pc_stats(&hg, 3, &gate);
    pc_solve(&gate, &away, &top, &r);
    CHECK(!r.valid && (r.flags & PC_NO_DRONE) && r.min_peak_db_x10 == 100 && r.active_db_x10 == 80,
          "no drone in the passage: the defaults and a warning");

    /* Clipping in the passage: lower the gain. */
    measure(&hg, 3, 40.0f, 2.0f, 2500, 5);                       /* 20 % clipped */
    pc_stats(&hg, 3, &gate);
    measure(&ha, 3, 12.0f, 1.5f, 2500, 0);
    pc_stats(&ha, 3, &away);
    pc_solve(&gate, &away, &away, &r);
    CHECK(r.valid && (r.flags & PC_CLIPPING) && r.gain_steps == -1, "clipping in the passage advises a lower gain");

    /* The drone barely seen away: raise the gain. */
    measure(&hg, 3, 20.0f, 2.0f, 2500, 0);
    pc_stats(&hg, 3, &gate);
    measure(&ha, 3, 0.5f, 0.5f, 2500, 0);
    pc_stats(&ha, 3, &away);
    pc_solve(&gate, &away, &away, &r);
    CHECK(r.valid && (r.flags & PC_AWAY_WEAK) && r.gain_steps == 1 && !(r.flags & PC_CLIPPING),
          "a drone hardly seen at 2 m advises a higher gain");

    /* Too little separation: warned, min_peak halfway. */
    measure(&hg, 3, 14.0f, 1.0f, 2500, 0);
    pc_stats(&hg, 3, &gate);
    measure(&ha, 3, 11.0f, 1.0f, 2500, 0);
    pc_stats(&ha, 3, &away);
    pc_solve(&gate, &away, &away, &r);
    CHECK(r.valid && (r.flags & PC_LOW_SEPARATION), "levels too close: a separation warning");
    CHECK(r.min_peak_db_x10 >= 5 * away.p95 - 10 && r.min_peak_db_x10 <= 5 * gate.p10 + 10, "min_peak falls between them anyway (1 dB of slack)");

    /* A different channel away: another drone. */
    measure(&hg, 3, 20.0f, 2.0f, 2500, 0);
    pc_stats(&hg, 3, &gate);
    measure(&ha, 3, 8.0f, 1.5f, 2500, 0);
    for (int i = 0; i < 2500; i++) pc_hist_add(&ha, 6, 24.0f);
    pc_stats(&ha, 3, &away);
    pc_stats(&ha, pc_strongest(&ha), &top);
    pc_solve(&gate, &away, &top, &r);
    CHECK(top.channel == 6 && (r.flags & PC_OTHER_CHANNEL), "another channel standing far higher away is flagged");

    /* A huge gap never goes over the ceiling; an empty measurement counts as no drone. */
    measure(&hg, 3, 60.0f, 1.0f, 500, 0);
    pc_stats(&hg, 3, &gate);
    measure(&ha, 3, 45.0f, 1.0f, 500, 0);
    pc_stats(&ha, 3, &away);
    pc_solve(&gate, &away, &away, &r);
    CHECK(r.min_peak_db_x10 <= 400, "min_peak never passes 40 dB");
    memset(&gate, 0, sizeof gate);
    pc_solve(&gate, &away, &away, &r);
    CHECK(!r.valid && (r.flags & PC_NO_DRONE), "an empty passage measurement is no drone");

    printf("%d/%d pass_calib checks passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
