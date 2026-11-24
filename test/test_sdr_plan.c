/* Host test for the sdr_rx LO planner (components/sdr_rx/sdr_plan.c): the Raceband pairs and
 * single plans and widscan58's group cases (CapnBry, a9fff97, firmware/test/test_dsp/test_main.c,
 * given here in kHz), and the integer FFT bin windows. Build/run (see CLAUDE.md):
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/sdr_rx -I../components/sdr_rx/include test_sdr_plan.c \
 *     ../components/sdr_rx/sdr_plan.c -o /tmp/t && /tmp/t
 */
#include "sdr_plan.h"
#include <stdio.h>

static int g_pass, g_fail;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { printf("ok   %s\n", name); g_pass++; }                    \
        else { printf("FAIL %s\n", name); g_fail++; }                         \
    } while (0)

static const uint32_t RACEBAND_KHZ[8] = {5658000, 5695000, 5732000, 5769000, 5806000, 5843000, 5880000, 5917000};

static unsigned khz_of(const uint32_t *mhz, unsigned n, uint32_t *khz) {
    for (unsigned j = 0; j < n; j++) khz[j] = mhz[j] * 1000u;
    return n;
}

static sdr_plan_t plan_mhz(const uint32_t *mhz, unsigned n) {
    uint32_t khz[SDR_RX_CHANNELS];
    const sdr_plan_cfg_t cfg = sdr_plan_cfg_default();
    sdr_plan_t p;
    sdr_plan_build(khz, khz_of(mhz, n, khz), SDR_PLAN_PAIRS, &cfg, &p);
    return p;
}

int main(void) {
    const sdr_plan_cfg_t cfg = sdr_plan_cfg_default();
    CHECK(cfg.hop_max_span_khz == 40000 && cfg.single_lo_offset_khz == -8000 && cfg.lo_step_khz == 1000,
          "default config: c5-race span, single offset, step");

    /* R1..R8 in pairs: 4 LOs, R(2k+1) 19 MHz below, R(2k+2) 18 MHz above. */
    sdr_plan_t p;
    sdr_plan_build(RACEBAND_KHZ, 8, SDR_PLAN_PAIRS, &cfg, &p);
    const uint32_t lo[4] = {5677000, 5751000, 5825000, 5899000};
    int ok = p.nhops == 4;
    for (unsigned h = 0; ok && h < 4; h++)
        ok = p.hop[h].lo_khz == lo[h] && p.hop[h].nch == 2 && p.hop[h].ch[0] == 2 * h && p.hop[h].ch[1] == 2 * h + 1 &&
             p.hop[h].offset_hz[0] == -19000000 && p.hop[h].offset_hz[1] == 18000000;
    CHECK(ok, "pairs: LOs 5677/5751/5825/5899 MHz, offsets -19/+18 MHz, channels in order");

    /* The same channels in another order give the same LOs, channels sorted by frequency. */
    const uint32_t shuffled[8] = {5917000, 5658000, 5843000, 5695000, 5880000, 5732000, 5806000, 5769000};
    sdr_plan_build(shuffled, 8, SDR_PLAN_PAIRS, &cfg, &p);
    ok = p.nhops == 4;
    for (unsigned h = 0; ok && h < 4; h++)
        ok = p.hop[h].lo_khz == lo[h] && shuffled[p.hop[h].ch[0]] < shuffled[p.hop[h].ch[1]];
    CHECK(ok && p.hop[0].ch[0] == 1 && p.hop[0].ch[1] == 3, "pairs: ch[] indexes the caller's list, sorted");

    /* Single: one LO per channel, 8 MHz below it. */
    sdr_plan_build(RACEBAND_KHZ, 8, SDR_PLAN_SINGLE, &cfg, &p);
    ok = p.nhops == 8;
    for (unsigned h = 0; ok && h < 8; h++)
        ok = p.hop[h].nch == 1 && p.hop[h].ch[0] == h && p.hop[h].lo_khz == RACEBAND_KHZ[h] - 8000 &&
             p.hop[h].offset_hz[0] == 8000000;
    CHECK(ok && p.hop[0].lo_khz == 5650000, "single: 8 LOs at ch - 8 MHz (R1 -> 5650, +8 MHz)");

    /* widscan58's whole bands: A, B and F take 3 LOs, R, E and L still 4. */
    const uint32_t bands[6][8] = {
        {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917}, {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725},
        {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866}, {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945},
        {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880}, {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621}};
    const unsigned hops[6] = {4, 3, 3, 4, 3, 4};
    ok = 1;
    for (unsigned b = 0; b < 6; b++) {
        p = plan_mhz(bands[b], 8);
        if (p.nhops != hops[b]) {
            printf("     band %u: %u hops, want %u\n", b, (unsigned)p.nhops, hops[b]);
            ok = 0;
        }
    }
    CHECK(ok, "bands R, A, B, E, F, L: 4, 3, 3, 4, 3, 4 hops");

    /* A8, R3, B1 within 8 MHz: one LO, DC 8 MHz below all three rather than in the 1 MHz gap. */
    const uint32_t close[3] = {5732, 5725, 5733};
    p = plan_mhz(close, 3);
    CHECK(p.nhops == 1 && p.hop[0].nch == 3 && p.hop[0].lo_khz == 5717000, "5732/5725/5733: one LO at 5717 MHz");
    CHECK(p.hop[0].ch[0] == 1 && p.hop[0].ch[1] == 0 && p.hop[0].ch[2] == 2, "5732/5725/5733: sorted by frequency");

    /* F1 F2 F3 (40 MHz span): one LO, 1 MHz off F2 since no gap is within reach. */
    const uint32_t f[3] = {5740, 5760, 5780};
    p = plan_mhz(f, 3);
    CHECK(p.nhops == 1 && p.hop[0].nch == 3 && p.hop[0].lo_khz == 5759000, "5740/5760/5780: one LO at 5759 MHz");

    /* B1, R3, B2, F2 (span 28): one LO in the 19 MHz gap. */
    const uint32_t four[4] = {5733, 5732, 5752, 5760};
    p = plan_mhz(four, 4);
    CHECK(p.nhops == 1 && p.hop[0].nch == 4 && p.hop[0].lo_khz == 5743000, "5733/5732/5752/5760: one LO at 5743 MHz");

    /* 41 MHz is wider than the limit: two LOs. */
    const uint32_t wide[2] = {5733, 5774};
    p = plan_mhz(wide, 2);
    CHECK(p.nhops == 2, "5733/5774 (41 MHz): two LOs");

    /* F8 and R7 share 5880 MHz: one LO, off the channel. */
    const uint32_t same[2] = {5880, 5880};
    p = plan_mhz(same, 2);
    CHECK(p.nhops == 1 && p.hop[0].nch == 2 && p.hop[0].lo_khz == 5872000, "5880/5880: one LO at 5872 MHz");

    /* More than SDR_RX_CHANNELS: the first 8 only. */
    uint32_t nine[9];
    for (unsigned j = 0; j < 9; j++) nine[j] = j < 8 ? RACEBAND_KHZ[j] : 5945000;
    sdr_plan_build(nine, 9, SDR_PLAN_PAIRS, &cfg, &p);
    CHECK(p.nhops == 4 && p.hop[3].lo_khz == 5899000, "nine channels: the first eight planned");

    /* Bin windows, nfft 64 at 80 MS/s (1.25 MHz bins): -19 MHz -> 13..20, +18 MHz -> 43..50. */
    sdr_band_t b = sdr_plan_band(64, 80000000, -19000000, 5000000);
    CHECK(b.r0 == 13 && b.r1 == 20, "band: -19 MHz +-5 at nfft 64 -> bins 13..20");
    b = sdr_plan_band(64, 80000000, 18000000, 5000000);
    CHECK(b.r0 == 43 && b.r1 == 50, "band: +18 MHz +-5 at nfft 64 -> bins 43..50");
    b = sdr_plan_band(64, 80000000, 8000000, 5000000);
    CHECK(b.r0 == 35 && b.r1 == 42, "band: +8 MHz (single) -> bins 35..42");
    b = sdr_plan_band(64, 80000000, 0, 5000000);
    CHECK(b.r0 == 28 && b.r1 == 36, "band: over DC -> bins 28..36 (DC 32 inside)");
    b = sdr_plan_band(256, 80000000, -19000000, 5000000);
    CHECK(b.r0 == 128 - 76 && b.r1 == 128 - 45, "band: -19 MHz at nfft 256 (312.5 kHz bins) -> 52..83");
    b = sdr_plan_band(128, 80000000, 18000000, 5000000);
    CHECK(b.r0 == 64 + 21 && b.r1 == 64 + 36, "band: +18 MHz at nfft 128 -> 85..100");
    b = sdr_plan_band(64, 80000000, 45000000, 5000000);
    CHECK(b.r0 == 63 && b.r1 == 63, "band: beyond Nyquist clamped to the last bin");
    b = sdr_plan_band(64, 80000000, -45000000, 5000000);
    CHECK(b.r0 == 0 && b.r1 == 0, "band: below -Nyquist clamped to bin 0");

    sdr_plan_build(RACEBAND_KHZ, 8, SDR_PLAN_PAIRS, &cfg, &p);
    sdr_plan_bands(&p, 64, 80000000, 5000000);
    ok = 1;
    for (unsigned h = 0; h < p.nhops; h++)
        ok &= p.hop[h].band[0].r0 == 13 && p.hop[h].band[0].r1 == 20 && p.hop[h].band[1].r0 == 43 &&
              p.hop[h].band[1].r1 == 50;
    CHECK(ok, "plan bands: every pair at 13..20 and 43..50");

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
