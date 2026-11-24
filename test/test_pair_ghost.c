/* Host test: the partner-channel ghost of PAIRS mode, end to end through the real DSP, and the
 * pair guard that removes it. A synthetic 10-bit C5 capture of LO 5825 (an analog FM VTX on R6,
 * +18 MHz; optionally a second one on R5, -19 MHz; receiver noise; a raw I/Q imbalance of 27.8 dB,
 * the board's boot verify at that LO; DC; each path rounded and clipped to the 10-bit rails) goes
 * through psd_compute, sdr_level_scale_q8 and sdr_band_level_qdb exactly as sdr_rx_step calls them,
 * then into the scan detector, fed one reading at a time (lt_scan.c before the guard) and through
 * sd_feed_hop. The other six channels read Gaussian floor readings.
 * What it shows (a model, not the board):
 *   - a 25 mW VTX swept past the antenna by hand, 15 cm at its closest, 1 m/s (the bench's ghosts:
 *     R5 rang 13.6-24.8 dB passes while R6 was at the ADC ceiling) clips the capture, and R5 rings a
 *     phantom pass when fed per reading; guarded it rings none, and R6's pass is the same;
 *   - a real R5 pilot 30 dB under a 600 mW R6 passing 0.3 m away, 200 ms behind it, is lost per
 *     reading (the phantom at R6's time takes the pass) and kept, on time, guarded;
 *   - mirrored, the lower channel strong: a clipping R5 rings a phantom on R6 per reading, none
 *     guarded (R6 held), and R5's pass is the same (600 mW at 0.3 m, then the hand-held case).
 * The VTX model: a PAL-like line (sync tip -1, a black-to-white ramp, 250 ns edges, AC coupled),
 * 12 MHz peak deviation (the deviation that fits the bench's ceiling reading), a 6.5 MHz audio
 * subcarrier at 1.5 MHz deviation. Build/run (see CLAUDE.md):
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/sdr_rx -I../components/sdr_rx/include \
 *     -I../components/pass_detector/include test_pair_ghost.c ../components/sdr_rx/sdr_psd.c \
 *     ../components/sdr_rx/sdr_level.c ../components/sdr_rx/sdr_plan.c \
 *     ../components/pass_detector/scan_detector.c ../components/pass_detector/pass_detector.c \
 *     -lm -o /tmp/t && /tmp/t
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scan_detector.h"
#include "sdr_level.h"
#include "sdr_plan.h"
#include "sdr_psd.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_pass, g_fail;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { printf("ok   %s\n", name); g_pass++; }                    \
        else { printf("FAIL %s\n", name); g_fail++; }                         \
    } while (0)

#define NCAP 256u            /* 4 x 64-point segments, as sdr_rx */
#define NFFT 64u
#define RATE 80e6
#define ROT_US (1e6 / 509.3) /* the board's rotation rate */
#define NOISE 1.06           /* counts rms per component: the floor reads -238 qdb */
#define DEV_HZ 12e6
#define IQ_G 1.08            /* Q gain and phase: a 27.8 dB raw image rejection */
#define IQ_PHI_DEG 1.5
#define E0_DBFS (-14.9)      /* envelope of 25 mW at 0.9 m: R6 40 dB over its floor */
#define T0_US 1000000
#define LEARN_S 3.5

/* splitmix64, Box-Muller */
static uint64_t g_s;
static uint64_t rnext(void)
{
    uint64_t z = (g_s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static double runi(void) { return ((double)(rnext() >> 11) + 0.5) / 9007199254740992.0; }
static double rgauss(void) { return sqrt(-2.0 * log(runi())) * cos(2.0 * M_PI * runi()); }

/* Composite video, 64 us line: sync 0..4.7 us at -1, black -0.4, a ramp to white over the active
 * line, 250 ns raised-cosine edges; AC coupled and scaled so the sync tip is -1. */
static double step(double t, double edge)
{
    const double x = (t - edge) / 0.25 + 0.5;
    return x <= 0.0 ? 0.0 : x >= 1.0 ? 1.0 : 0.5 - 0.5 * cos(M_PI * x);
}
static double video_level(double t)
{
    const double sync = step(t, 0.0) - step(t, 4.7) + step(t, 64.0);
    const double ramp = t < 10.5 ? 0.0 : t < 62.35 ? 1.4 * (t - 10.5) / (62.35 - 10.5) : 1.4;
    return -0.4 - 0.6 * sync + ramp * (1.0 - step(t, 62.35));
}
static double g_vmean = 2.0;
static double video(double t_s)
{
    if (g_vmean > 1.5) {
        double s = 0.0;
        for (unsigned k = 0; k < 64000u; k++) s += video_level(64.0 * (k + 0.5) / 64000.0);
        g_vmean = s / 64000.0;
    }
    double ph = t_s * 15625.0;
    ph -= floor(ph);
    return (video_level(64.0 * ph) - g_vmean) / (1.0 + g_vmean);
}

/* One VTX into re/im (the display frame; the raw words carry its conjugate). */
static void vtx(double off_hz, double env_dbfs, double t0_s, double *re, double *im)
{
    const double a = 512.0 * pow(10.0, env_dbfs / 20.0);
    double th = 2.0 * M_PI * runi();
    const double aph = 2.0 * M_PI * runi();
    for (unsigned n = 0; n < NCAP; n++) {
        const double t = t0_s + n / RATE;
        const double f = off_hz + DEV_HZ * video(t) + 1.5e6 * sin(2.0 * M_PI * 6.5e6 * t + aph);
        re[n] += a * cos(th);
        im[n] -= a * sin(th);
        th += 2.0 * M_PI * f / RATE;
    }
}
static int adc10(double v)
{
    const long q = lrint(v);
    return q > 511 ? 511 : q < -512 ? -512 : (int)q;
}
/* A capture with R6 at env6 and R5 at env5 (dBFS, < -200: absent). */
static void capture(double env6, double env5, double t_s, uint32_t *w)
{
    double re[NCAP] = {0}, im[NCAP] = {0};
    if (env6 > -200.0) vtx(18e6, env6, t_s, re, im);
    if (env5 > -200.0) vtx(-19e6, env5, t_s + 7.3e-6, re, im);
    const double phi = IQ_PHI_DEG * M_PI / 180.0;
    for (unsigned n = 0; n < NCAP; n++) {
        const double x = re[n] + NOISE * rgauss(), y = im[n] + NOISE * rgauss();
        const int i = adc10(x + 3.0), q = adc10(IQ_G * (cos(phi) * y + sin(phi) * x) - 2.0);
        w[n] = ((uint32_t)i & 0x3ffu) | (((uint32_t)q & 0x3ffu) << 10);
    }
}

static _Alignas(16) uint8_t g_ws[PSD_WS_BYTES];
static sdr_plan_t g_plan;
static unsigned g_hop = 99;   /* the plan hop of LO 5825 */

/* A pass: env(t) = E0 + 10 log10(mw / 25) - 10 log10((d0^2 + (v (t - t0))^2) / 0.9^2). */
typedef struct {
    double mw, d0, v, t0;
} pass_t;
static double pass_env(const pass_t *p, double t)
{
    if (p->mw <= 0.0) return -999.0;
    const double x = p->v * (t - p->t0);
    return E0_DBFS + 10.0 * log10(p->mw / 25.0) - 10.0 * log10((p->d0 * p->d0 + x * x) / 0.81);
}

typedef struct {
    unsigned n5, n6, sat, held5, held6;   /* held: readings the guard held */
    double t5[8], t6[8];
    float db5[8], db6[8];
} out_t;

/* 3.5 s of quiet (the rests settle; floor captures of the pair), then dur_s of the scene. */
static void run(const pass_t *r6, const pass_t *r5, int guard, uint64_t seed, double dur_s, out_t *o)
{
    static scan_detector_t sd;
    const pd_config_t c = sd_config_default();
    sd_init(&sd, &c);
    memset(o, 0, sizeof(*o));
    g_s = seed;
    psd_iq_t iq = {0};
    const sdr_plan_hop_t *hp = &g_plan.hop[g_hop];
    uint32_t w[NCAP];
    for (unsigned rot = 0;; rot++) {
        const double t_rot = T0_US + rot * ROT_US;
        if ((t_rot - T0_US) * 1e-6 > LEARN_S + dur_s) break;
        for (unsigned h = 0; h < g_plan.nhops; h++) {
            const double t_cap = t_rot + h * ROT_US / g_plan.nhops;
            const double ts = (t_cap - T0_US) * 1e-6 - LEARN_S;
            sd_reading_t r[2];
            if (h == g_hop) {
                capture(ts >= 0.0 ? pass_env(r6, ts) : -999.0, ts >= 0.0 ? pass_env(r5, ts) : -999.0, t_cap * 1e-6, w);
                psd_result_t psd;
                (void)psd_compute(w, NCAP, NFFT, 4, (float)RATE, true, 40e6f, 1, &iq, &psd);   /* as sdr_rx_step */
                const int32_t sq8 = sdr_level_scale_q8(&psd);
                for (unsigned j = 0; j < 2; j++) {
                    r[j].level_qdb = sdr_band_level_qdb(&psd, hp->band[j], sq8, &r[j].flags);
                    r[j].channel = hp->ch[j];
                    r[j].t_us = (int64_t)llround(t_cap);
                }
                o->sat += psd.saturated > 0;
            } else {
                for (unsigned j = 0; j < 2; j++)
                    r[j] = (sd_reading_t){(int64_t)llround(t_cap), (int16_t)lrint(-238.0 + 4.8 * rgauss()), g_plan.hop[h].ch[j], 0};
            }
            pd_event_t ev[2];
            unsigned rang = 0, held = 0;
            if (guard) {
                rang = sd_feed_hop(&sd, h, r, 2, ev, &held);
                for (unsigned j = 0; j < 2; j++) {
                    o->held5 += (held >> j & 1u) && r[j].channel == 4u;
                    o->held6 += (held >> j & 1u) && r[j].channel == 5u;
                }
            } else {
                for (unsigned j = 0; j < 2; j++)
                    if (sd_feed(&sd, r[j].channel, r[j].level_qdb, r[j].flags, r[j].t_us, &ev[j])) rang |= 1u << j;
            }
            for (unsigned j = 0; j < 2; j++) {
                if (!(rang & (1u << j))) continue;
                const double t = ev[j].peak_ms * 1e-3 - (T0_US * 1e-6 + LEARN_S);
                if (ev[j].channel == 4u && o->n5 < 8u) {
                    o->db5[o->n5] = ev[j].peak_db;
                    o->t5[o->n5++] = t;
                }
                if (ev[j].channel == 5u && o->n6 < 8u) {
                    o->db6[o->n6] = ev[j].peak_db;
                    o->t6[o->n6++] = t;
                }
            }
        }
        sd_rotation(&sd, (int64_t)llround(t_rot + ROT_US));
    }
}

int main(void)
{
    CHECK(psd_init(g_ws, sizeof(g_ws)) == ESP_OK, "psd_init");
    metrics_init();
    static const uint16_t race[8] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
    uint32_t khz[8];
    for (unsigned j = 0; j < 8; j++) khz[j] = race[j] * 1000u;
    const sdr_plan_cfg_t pc = sdr_plan_cfg_default();
    sdr_plan_build(khz, 8, SDR_PLAN_PAIRS, &pc, &g_plan);
    sdr_plan_bands(&g_plan, NFFT, 80000000u, 5000000u);   /* as sdr_rx's plan_rebuild */
    for (unsigned h = 0; h < g_plan.nhops; h++)
        if (g_plan.hop[h].lo_khz == 5825000u) g_hop = h;
    CHECK(g_hop < g_plan.nhops && g_plan.hop[g_hop].nch == 2 && g_plan.hop[g_hop].ch[0] == 4 && g_plan.hop[g_hop].ch[1] == 5 &&
              g_plan.hop[g_hop].band[0].r0 == 13 && g_plan.hop[g_hop].band[1].r0 == 43,
          "LO 5825 carries R5 (bins 13..20) and R6 (bins 43..50)");

    /* 1. The bench: 25 mW swept past the antenna by hand, 15 cm at its closest, 1 m/s. */
    {
        const pass_t r6 = {25.0, 0.15, 1.0, 2.0}, none = {0};
        out_t base, gd;
        run(&r6, &none, 0, 11, 4.0, &base);
        run(&r6, &none, 1, 11, 4.0, &gd);
        printf("     hand-held 25 mW at 15 cm: %u clipped captures; per reading R5 %u pass(es) (%.1f dB), R6 %u (%.1f dB); guarded R5 %u, R6 %u\n",
               base.sat, base.n5, base.n5 ? (double)base.db5[0] : 0.0, base.n6, base.n6 ? (double)base.db6[0] : 0.0, gd.n5, gd.n6);
        CHECK(base.sat > 50u, "the capture clips (about 0.2 s of saturated captures)");
        CHECK(base.n5 >= 1 && base.db5[0] >= 13.0f && base.db5[0] <= 25.0f, "fed per reading, R5 rings a phantom pass of the bench's size (13.6-24.8 dB)");
        CHECK(gd.n5 == 0, "through sd_feed_hop, R5 rings none");
        CHECK(base.n6 == 1 && gd.n6 == 1 && base.t6[0] == gd.t6[0], "R6's own pass is the same pass");
    }

    /* 2. A 600 mW R6 passes 0.3 m away at 20 m/s; a real R5 pilot 30 dB under it follows 200 ms behind. */
    {
        const pass_t r6 = {600.0, 0.3, 20.0, 2.0}, r5 = {0.6, 0.3, 20.0, 2.2}, none = {0};
        out_t base, gd, alone;
        run(&r6, &r5, 0, 12, 4.0, &base);
        run(&r6, &r5, 1, 12, 4.0, &gd);
        run(&none, &r5, 0, 12, 4.0, &alone);
        printf("     R5 30 dB under, +200 ms: alone at %+.0f ms; per reading %u pass(es), first at %+.0f ms; guarded %u at %+.0f ms\n",
               alone.n5 ? (alone.t5[0] - 2.2) * 1e3 : 0.0, base.n5, base.n5 ? (base.t5[0] - 2.2) * 1e3 : 0.0, gd.n5,
               gd.n5 ? (gd.t5[0] - 2.2) * 1e3 : 0.0);
        CHECK(alone.n5 == 1, "R5 alone rings its pass");
        CHECK(base.n5 >= 1 && fabs(base.t5[0] - alone.t5[0]) > 0.1, "per reading, R5's first pass is the phantom at R6's time");
        CHECK(gd.n5 == 1 && fabs(gd.t5[0] - alone.t5[0]) <= 0.03, "guarded, R5 rings once, within 30 ms of R5 alone");
        CHECK(gd.n6 == 1 && base.n6 == 1 && gd.t6[0] == base.t6[0], "and R6's pass is the same");
    }

    /* 3. Mirrored: the lower channel strong. R5 (-19 MHz) passes 0.3 m away at 600 mW and 20 m/s,
     *    then hand-held at 25 mW (15 cm, 1 m/s), with R6 alone in the air: R6 reads R5's clipping
     *    products. The guard compares the pair both ways: R6 is held and rings none, and R5 (never
     *    held) rings the same pass. */
    {
        const pass_t fast = {600.0, 0.3, 20.0, 2.0}, hand = {25.0, 0.15, 1.0, 2.0}, none = {0};
        out_t fb, fg, hb, hg;
        run(&none, &fast, 0, 13, 4.0, &fb);
        run(&none, &fast, 1, 13, 4.0, &fg);
        run(&none, &hand, 0, 14, 4.0, &hb);
        run(&none, &hand, 1, 14, 4.0, &hg);
        printf("     mirrored, 600 mW R5 at 0.3 m: %u clipped captures; per reading R6 %u pass(es) (%.1f dB); guarded R6 %u, "
               "%u held\n", fb.sat, fb.n6, fb.n6 ? (double)fb.db6[0] : 0.0, fg.n6, fg.held6);
        printf("     mirrored, hand-held 25 mW R5 at 15 cm: %u clipped captures; per reading R6 %u pass(es) (%.1f dB); "
               "guarded R6 %u, %u held\n", hb.sat, hb.n6, hb.n6 ? (double)hb.db6[0] : 0.0, hg.n6, hg.held6);
        CHECK(fb.sat > 20u && fb.n6 >= 1 && fb.db6[0] >= 13.0f && hb.sat > 50u && hb.n6 >= 1 && hb.db6[0] >= 13.0f,
              "mirrored: a clipping R5 rings a phantom R6 pass per reading");
        CHECK(fg.n6 == 0 && hg.n6 == 0 && fg.held6 > 0 && hg.held6 > 0, "through sd_feed_hop R6 is held and rings none");
        CHECK(fb.n5 == 1 && fg.n5 == 1 && fb.t5[0] == fg.t5[0] && hb.n5 == 1 && hg.n5 == 1 && hb.t5[0] == hg.t5[0] &&
                  fg.held5 == 0 && hg.held5 == 0,
              "and R5's pass is the same pass, never held");
    }

    printf("%d/%d pair_ghost checks passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
