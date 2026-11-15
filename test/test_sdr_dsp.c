/* Host test for the sdr_rx DSP (components/sdr_rx): the radix-4 FFT kernel against a
 * double-precision DFT and metrics_db/metrics_code_q8 (after widscan58, CapnBry, a9fff97,
 * firmware/test/test_dsp/test_main.c), then psd_compute and the band level on synthetic
 * captures, the moment-based IQ correction, psd_moments_add/psd_iq_irr_db and saturation.
 * These check the formulas and the estimators, not the hardware. Build/run (see CLAUDE.md):
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/sdr_rx -I../components/sdr_rx/include test_sdr_dsp.c \
 *     ../components/sdr_rx/sdr_psd.c ../components/sdr_rx/sdr_level.c -lm -o /tmp/t && /tmp/t
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fft_r4_sc16.h"
#include "sdr_level.h"
#include "sdr_psd.h"
#include "sdr_radio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_pass, g_fail;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { printf("ok   %s\n", name); g_pass++; }                    \
        else { printf("FAIL %s\n", name); g_fail++; }                         \
    } while (0)

#define RATE 80e6
#define NCAP 256u
#define NFFT 64u

/* Deterministic xorshift32 and Box-Muller, in double. */
static uint32_t g_rng = 1;
static void seed(uint32_t s) { g_rng = s ? s : 1u; }
static uint32_t xs32(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static double uni(void) { return (xs32() + 0.5) / 4294967296.0; } /* (0, 1) */
static double gauss(void) { return sqrt(-2.0 * log(uni())) * cos(2.0 * M_PI * uni()); }

static int clamp10(double v) {
    long r = lrint(v);
    return r > 511 ? 511 : r < -512 ? -512 : (int)r;
}
static uint32_t pack(int i, int q) { return ((uint32_t)i & 0x3ffu) | (((uint32_t)q & 0x3ffu) << 10); }

/* A raw capture (before psd_compute's conjugation): a complex tone A e^{j(2 pi f t + phase)}
 * plus Gaussian noise of rms `noise` per component; the Q path scaled by qgain. */
static void synth(uint32_t *w, unsigned n, double amp, double f_hz, double phase, double qgain, double noise) {
    for (unsigned j = 0; j < n; j++) {
        const double a = 2.0 * M_PI * f_hz * j / RATE + phase;
        const double i = amp * cos(a) + noise * gauss();
        const double q = qgain * (amp * sin(a) + noise * gauss());
        w[j] = pack(clamp10(i), clamp10(q));
    }
}

static _Alignas(16) uint8_t g_ws[PSD_WS_BYTES];
static uint32_t g_cap[NCAP];

static esp_err_t psd(const uint32_t *w, psd_iq_t *iq, psd_result_t *r) {
    return psd_compute(w, NCAP, NFFT, 4, (float)RATE, true, 40e6f, 1, iq, r);
}

/* The two channels of a pair LO at nfft 64 (test_sdr_plan.c checks these windows). */
static const sdr_band_t LOW = {13, 20};   /* -19 MHz +-5 */
static const sdr_band_t HIGH = {43, 50};  /* +18 MHz +-5 */
static int16_t level(const psd_result_t *r, sdr_band_t b, uint8_t *flags) {
    return sdr_band_level_qdb(r, b, sdr_level_scale_q8(r), flags);
}

/* Error of fft_r4_sc16 vs the exact X/N, in LSB: rms and mean (bias). */
static void fft_error(unsigned n, double amp, double *rms, double *mean, double *max) {
    static int16_t x[2 * 1024], in[2 * 1024];
    static uint16_t pos[1024];
    fft_r4_input_pos(pos, n);
    double e2 = 0, e1 = 0, emax = 0;
    long cnt = 0;
    for (unsigned it = 0; it < 8 * 1024 / n; it++) { /* same bin count for every n */
        for (unsigned j = 0; j < n; j++) {
            for (int c = 0; c < 2; c++) {
                double v = amp * gauss();
                in[2 * j + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
            }
            x[2 * pos[j]] = in[2 * j];
            x[2 * pos[j] + 1] = in[2 * j + 1];
        }
        fft_r4_sc16(x, n);
        for (unsigned k = 0; k < n; k++) {
            double sr = 0, si = 0;
            for (unsigned j = 0; j < n; j++) {
                double a = -2 * M_PI * (double)((j * k) % n) / n;
                sr += in[2 * j] * cos(a) - in[2 * j + 1] * sin(a);
                si += in[2 * j] * sin(a) + in[2 * j + 1] * cos(a);
            }
            double dr = x[2 * k] - sr / n, di = x[2 * k + 1] - si / n;
            e2 += dr * dr + di * di;
            e1 += dr + di;
            emax = fmax(emax, fmax(fabs(dr), fabs(di)));
            cnt += 2;
        }
    }
    *rms = sqrt(e2 / cnt);
    *mean = e1 / cnt;
    *max = emax;
}

static void test_fft(void) {
    static uint16_t pos[1024];
    static uint8_t seen[1024];
    int ok = 1;
    for (unsigned n = 4; n <= 1024; n *= 2) {
        fft_r4_input_pos(pos, n);
        memset(seen, 0, sizeof(seen));
        for (unsigned j = 0; j < n; j++) seen[pos[j]]++;
        for (unsigned j = 0; j < n; j++) ok &= seen[j] == 1;
    }
    CHECK(ok, "fft_r4_input_pos is a permutation (n = 4..1024)");

    /* psd.c block-scales the input to ~2^13; at a few LSB, twiddle-free butterflies hit exact
     * rounding ties more often and the bias grows to ~0.1 LSB, still negligible. */
    const unsigned sizes[] = {64, 128, 256};
    ok = 1;
    seed(12345);
    for (unsigned s = 0; s < 3; s++)
        for (double amp = 30; amp <= 3000; amp *= 10) {
            double rms, mean, max;
            fft_error(sizes[s], amp, &rms, &mean, &max);
            if (!(rms < 0.42 && fabs(mean) < 0.04 && max < 3.0)) {
                printf("     n=%u amp=%.0f rms=%.3f mean=%.4f max=%.2f\n", sizes[s], amp, rms, mean, max);
                ok = 0;
            }
        }
    CHECK(ok, "fft_r4_sc16 vs DFT at 64/128/256: rms < 0.42, |mean| < 0.04, max < 3 LSB");

    /* A full-scale tone at a bin: |X/N| near 32767 at that bin only, without wrapping. */
    enum { N = 1024 };
    static int16_t x[2 * N];
    fft_r4_input_pos(pos, N);
    for (unsigned j = 0; j < N; j++) {
        double a = 2 * M_PI * 37 * j / N;
        x[2 * pos[j]] = (int16_t)lrint(32767 * cos(a));
        x[2 * pos[j] + 1] = (int16_t)lrint(32767 * sin(a));
    }
    fft_r4_sc16(x, N);
    ok = abs(x[2 * 37] - 32767) <= 4 && abs(x[2 * 37 + 1]) <= 4;
    for (unsigned k = 0; k < N; k++)
        if (k != 37) ok &= abs(x[2 * k]) <= 4;
    CHECK(ok, "fft_r4_sc16: full-scale tone at N = 1024, no overflow");
}

static void test_metrics(void) {
    metrics_init();
    seed(777);
    double worst = 0;
    for (int it = 0; it < 100000; it++) {
        float v = (float)(uni() * pow(10, (double)(xs32() % 380) / 10.0 - 19));
        if (v <= 1e-20f) continue;
        double err = fabs(metrics_db(v) - 10 * log10((double)v));
        if (err > worst) worst = err;
    }
    CHECK(worst < 2e-4 && metrics_db(0) == -200.0f, "metrics_db within 2e-4 dB, 0 -> -200");
    worst = 0;
    for (double v = 1; v < 1.8e19; v *= 1.0137) {
        uint64_t u = (uint64_t)v;
        double err = metrics_code_q8(u) / 256.0 - 40 * log10((double)u);
        if (fabs(err) > worst) worst = fabs(err);
    }
    CHECK(worst < 0.02 && metrics_code_q8(1) == 0, "metrics_code_q8 within 0.02 code, 1 -> 0");
}

static void test_levels(void) {
    psd_result_t r;
    uint8_t f_low = 0xff, f_high = 0xff;
    /* R1, 19 MHz below its LO: raw +19 MHz (the conjugation maps it to -19 MHz), amplitude 256
     * (-6.02 dBFS), noise rms 4 per component. */
    seed(1);
    synth(g_cap, NCAP, 256, +19e6, 0.3, 1.0, 4.0);
    CHECK(psd(g_cap, NULL, &r) == ESP_OK && r.nfft == NFFT && r.segments == 4 && !r.iq_applied,
          "psd_compute: 256 samples, nfft 64, 4 segments, no IQ correction");
    CHECK(fabsf(r.enbw_bins - 1.5f) < 0.01f, "psd_compute: Hann ENBW 1.5 bins");
    const int16_t low = level(&r, LOW, &f_low), high = level(&r, HIGH, &f_high);
    printf("     R1 tone: low %d qdb, high %d qdb\n", low, high);
    CHECK(low >= -26 && low <= -22, "band level: R1 tone at -6.0 +- 0.5 dBFS (-24 +- 2 qdb)");
    CHECK(high != SDR_LEVEL_NONE && low - high >= 30 * SDR_QDB_PER_DB, "band level: the +18 MHz window >= 30 dB lower");
    CHECK(f_low == 0 && f_high == 0, "band level: no flags off DC, unsaturated");
    uint8_t f_dc = 0;
    const int16_t dc = level(&r, (sdr_band_t){28, 36}, &f_dc);
    CHECK(dc != SDR_LEVEL_NONE && f_dc == SDR_RD_DC_MASKED, "band level: a window over DC is flagged DC_MASKED");
    uint8_t f_empty = 0;
    CHECK(level(&r, (sdr_band_t){20, 13}, &f_empty) == SDR_LEVEL_NONE && f_empty == 0, "band level: empty window -> NONE");

    /* R2, 18 MHz above: raw -18 MHz lands in the high window. */
    seed(2);
    synth(g_cap, NCAP, 256, -18e6, 1.1, 1.0, 4.0);
    psd(g_cap, NULL, &r);
    CHECK(level(&r, HIGH, NULL) >= -26 && level(&r, HIGH, NULL) <= -22 &&
              level(&r, HIGH, NULL) - level(&r, LOW, NULL) >= 30 * SDR_QDB_PER_DB,
          "band level: R2 tone in the +18 MHz window");

    /* All-zero capture: no power anywhere. */
    for (unsigned j = 0; j < NCAP; j++) g_cap[j] = 0;
    psd(g_cap, NULL, &r);
    CHECK(level(&r, LOW, NULL) == SDR_LEVEL_NONE, "band level: zero power -> SDR_LEVEL_NONE");

    /* Image without correction: Q x1.2 puts the tone's image at 20 log10(0.1/1.1) = -20.8 dB in
     * the partner's window (R1's image at +19 MHz, 1 MHz from R2). */
    seed(3);
    synth(g_cap, NCAP, 256, +19e6, 0.7, 1.2, 4.0);
    psd(g_cap, NULL, &r);
    const double image_off = (level(&r, HIGH, NULL) - level(&r, LOW, NULL)) / (double)SDR_QDB_PER_DB;
    printf("     image without correction: %.2f dB\n", image_off);
    CHECK(fabs(image_off + 20.8) <= 1.5, "image without correction: -20.8 +- 1.5 dB");

    /* With the correction: 8 captures in a row (fresh noise and phase each), noise rms 0.5; the
     * image in the last one >= 45 dB under the tone. */
    psd_iq_t iq = {0};
    seed(4);
    for (int c = 0; c < 8; c++) {
        synth(g_cap, NCAP, 256, +19e6, 2.0 * M_PI * uni(), 1.2, 0.5);
        psd(g_cap, &iq, &r);
    }
    const double image_on = (level(&r, HIGH, NULL) - level(&r, LOW, NULL)) / (double)SDR_QDB_PER_DB;
    printf("     image with correction: %.2f dB (state gain %.4f)\n", image_on, sqrt(iq.qq / iq.ii));
    CHECK(r.iq_applied && iq.init, "IQ correction: a plausible estimate is applied");
    CHECK(image_on <= -45.0, "image with correction: <= -45 dB after 8 captures");

    /* An implausible estimate (no Q at all) is not applied. */
    psd_iq_t dead = {0};
    seed(5);
    synth(g_cap, NCAP, 256, +19e6, 0.0, 0.0, 4.0);
    psd(g_cap, &dead, &r);
    CHECK(dead.init && !r.iq_applied, "IQ correction: an implausible estimate (dead Q) is not applied");
}

static void test_irr(void) {
    psd_iq_t m = {1.0f, 1.44f, 0.0f, true};
    CHECK(fabsf(psd_iq_irr_db(&m) - 20.83f) <= 0.01f, "psd_iq_irr_db {1, 1.44, 0} = 20.83 dB");
    m = (psd_iq_t){1.0f, 1.0f, 0.0f, true};
    CHECK(psd_iq_irr_db(&m) == 99.9f, "psd_iq_irr_db {1, 1, 0} = 99.9 (clamp)");
    m = (psd_iq_t){0.0f, 1.0f, 0.0f, true};
    CHECK(psd_iq_irr_db(&m) == 0.0f, "psd_iq_irr_db {0, 1, 0} = 0");
    /* A phase error against the formula as written: rho 0.1 -> ~26 dB. */
    m = (psd_iq_t){1.0f, 1.21f, 0.11f, true};
    const double g = 1.1, rho = 0.11 / 1.1, s = 2 * g * sqrt(1 - rho * rho);
    const double want = 10 * log10((1 + s + g * g) / (1 - s + g * g));
    CHECK(fabs(psd_iq_irr_db(&m) - want) < 0.01, "psd_iq_irr_db with a phase error matches the formula");

    /* Plain sums over 64 captures (16384 samples) of complex noise, rms 20, 20 seeds. */
    int ok_bad = 1, ok_good = 1;
    double min_good = 1e9, worst_bad = 0;
    for (uint32_t s = 1; s <= 20; s++) {
        for (int balanced = 0; balanced < 2; balanced++) {
            sdr_moments_t sum = {0};
            seed(1000 + 37 * s + (uint32_t)balanced);
            for (int c = 0; c < 64; c++) {
                synth(g_cap, NCAP, 0.0, 0.0, 0.0, balanced ? 1.0 : 1.2, 20.0);
                if (psd_moments_add(g_cap, NCAP, NFFT, &sum)) ok_bad = ok_good = 0;
            }
            const psd_iq_t mean = psd_moments_mean(&sum);
            const float irr = psd_iq_irr_db(&mean);
            if (balanced) {
                if (irr < min_good) min_good = irr;
                ok_good &= irr >= 35.0f && sum.segments == 256 && sum.nfft == NFFT && sum.saturated == 0;
            } else {
                if (fabs(irr - 20.8) > worst_bad) worst_bad = fabs(irr - 20.8);
                ok_bad &= fabsf(irr - 20.8f) <= 1.5f;
            }
        }
    }
    printf("     noise IRR: balanced min %.1f dB, Q x1.2 worst |irr - 20.8| %.2f dB\n", min_good, worst_bad);
    CHECK(ok_bad, "moments IRR from noise, Q x1.2: 20.8 +- 1.5 dB on 20 seeds");
    CHECK(ok_good, "moments IRR from noise, balanced: >= 35 dB on 20 seeds");

    sdr_moments_t a = {0};
    synth(g_cap, NCAP, 0.0, 0.0, 0.0, 1.0, 20.0);
    psd_moments_add(g_cap, NCAP, NFFT, &a);
    const sdr_moments_t before = a;
    CHECK(psd_moments_add(g_cap, NCAP, 128, &a) == 0 && a.segments == before.segments && a.cii == before.cii,
          "psd_moments_add: another nfft adds nothing");
}

static void test_saturation(void) {
    /* 10 samples at +511 and 10 at -512 (I), the rest small. */
    seed(9);
    synth(g_cap, NCAP, 0.0, 0.0, 0.0, 1.0, 10.0);
    for (unsigned j = 0; j < 10; j++) {
        g_cap[5 + 7 * j] = pack(511, 0);
        g_cap[100 + 9 * j] = pack(-512, 3);
    }
    psd_result_t r;
    psd_iq_t iq = {123.0f, 456.0f, 7.0f, true};
    const psd_iq_t was = iq;
    CHECK(psd(g_cap, &iq, &r) == ESP_OK && r.saturated == 20, "saturation: both rails counted (20 samples)");
    CHECK(iq.ii == was.ii && iq.qq == was.qq && iq.iq == was.iq && iq.init,
          "saturation: the psd_iq_t state is left unchanged");
    uint8_t f = 0;
    level(&r, LOW, &f);
    CHECK(f & SDR_RD_SATURATED, "saturation: the band level is flagged SATURATED");
    sdr_moments_t m = {0};
    CHECK(psd_moments_add(g_cap, NCAP, NFFT, &m) == 20 && m.saturated == 1 && m.segments == 0 && m.cii == 0,
          "saturation: psd_moments_add skips the capture and counts it");
    /* One step inside the rails: not saturated. */
    for (unsigned j = 0; j < 10; j++) {
        g_cap[5 + 7 * j] = pack(510, 0);
        g_cap[100 + 9 * j] = pack(-511, 3);
    }
    psd(g_cap, NULL, &r);
    CHECK(r.saturated == 0, "saturation: 510 and -511 are not rail hits");
}

int main(void) {
    CHECK(psd_init(g_ws, sizeof(g_ws) - 1) == ESP_ERR_INVALID_SIZE, "psd_init refuses a short workspace");
    CHECK(psd_init(g_ws, sizeof(g_ws)) == ESP_OK, "psd_init on a PSD_WS_BYTES workspace");
    metrics_init();
    test_fft();
    test_metrics();
    test_levels();
    test_irr();
    test_saturation();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
