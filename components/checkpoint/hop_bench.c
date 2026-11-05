/* Hop bench (console K then s), bench only: can one C5 scan Raceband R1-R8?
 * It times ways of hopping between the channels and how long the IQ power takes
 * to settle after each (docs/bench.md, GitHub issue #1):
 *   full            rf_set_channel(): the Wi-Fi driver, the frequency, the whole lock
 *   direct          phy_set_freq() alone: fast, but the gain is no longer held
 *   force           the same, then the gain forced
 *   agc+force       + phy_disable_agc()
 *   rfagc+force     + phy_rfagc_disable()
 *   direct+gain     + both
 *   direct+restore  the same as direct, then the lock replayed through rf_set_filter()
 * and each step of direct+gain on its own. On the board `force` wins: phy_set_freq()
 * drops the forced gain, and forcing it again is all it takes; phy_rfagc_disable()
 * alone costs 3.4 ms for nothing. The direct hops call libphy past rf.c:
 * operator-approved, on this bench only.
 * It owns the receiver like scan() does (receiver_pause + s_scan_active, so the
 * control loop stops reading windows and steering AFC), at scan()'s fixed gain,
 * then restores the channel, offset and gain. No detection meanwhile, and nothing
 * is saved. The protected receive chain is not edited: only its public calls and,
 * for the direct hops, the same libphy functions rf.c uses. A one-shot priority-1
 * task with its buffers on the heap, gone when the bench ends. */
#include "hop_bench.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hop_stats.h"
#include "phy.h"
#include "receiver.h"
#include "rf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* libphy, as in rf.c */
extern void phy_set_freq(uint16_t freq_mhz, int offset);
extern void phy_disable_agc(void);
extern void phy_rfagc_disable(void);

#define BENCH_PRIORITY  1u              /* below every receiver task */
#define BENCH_STACK     6144u
#define BENCH_GAIN      52u             /* scan()'s: every channel compared at one gain */
#define CHANNELS        8u              /* R1..R8 are indices 0..7 */
#define DIRECT_BASE     3u              /* R4: mid band, the Wi-Fi centre direct hops stay on */
#define SWEEPS          4u
#define TRACE_POINTS    100u
#define TRACE_STEP_US   100u            /* a floor: a point takes as long as it takes */
#define TAIL_POINTS     10u
#define DUMP_POINTS     30u             /* of the VTX channel's trace, printed */
#define STEP_SAMPLES    32u
#define WINDOW_US       ((uint32_t)(CONTROL_BYTES * 1000000ull / IQ_RATE_HZ))
#define FAR_SETTLE_MS   2u
#define LATE_MS         20u             /* a drift past the trace shows here */

_Static_assert(TRACE_POINTS <= HS_TRACE_MAX && TAIL_POINTS <= HS_TAIL_MAX && DUMP_POINTS <= TRACE_POINTS, "hop bench sizes");

enum { M_FULL, M_DIRECT, M_FORCE, M_AGC, M_RFAGC, M_GAIN, M_RESTORE, METHODS };
static const struct { const char *name; bool agc_off, rfagc_off, force, restore; } k_method[METHODS] = {
    [M_FULL]    = {"full", false, false, false, false},
    [M_DIRECT]  = {"direct", false, false, false, false},
    [M_FORCE]   = {"force", false, false, true, false},
    [M_AGC]     = {"agc+force", true, false, true, false},
    [M_RFAGC]   = {"rfagc+force", false, true, true, false},
    [M_GAIN]    = {"direct+gain", true, true, true, false},
    [M_RESTORE] = {"direct+restore", false, false, false, true},
};

enum { S_FREQ, S_AGC, S_RFAGC, S_FORCE, S_COPY, S_POWER, STEPS };
static const char *const k_step[STEPS] = {"set_freq", "agc_off", "rfagc_off", "force_gain", "copy", "power"};

typedef struct {
    uint8_t window[CONTROL_BYTES];
    uint32_t t_us[TRACE_POINTS], power[TRACE_POINTS];
    uint32_t hop_us[METHODS][CHANNELS][SWEEPS], settle_us[METHODS][CHANNELS][SWEEPS];
    uint32_t level[METHODS][CHANNELS], late[METHODS][CHANNELS];
    int clip_pm[METHODS][CHANNELS];
    uint32_t step_us[STEPS][STEP_SAMPLES];
    uint32_t dump_t[METHODS][DUMP_POINTS], dump_p[METHODS][DUMP_POINTS];
    uint8_t dump_ch[METHODS], dump_n[METHODS];
    uint32_t cpu_us;                    /* median copy + power of a window */
    unsigned failed;
} bench_t;

static volatile bool s_running;

/* Mean IQ power of a window, DC removed, x256 (4-bit I and Q as measure_window). */
static uint32_t window_power(const uint8_t *s, size_t n)
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

static void hop(bench_t *b, unsigned method, unsigned ch)
{
    if (method == M_FULL) {
        if (rf_set_channel(ch) != ESP_OK) b->failed++;
        return;
    }
    phy_begin();
    phy_set_freq(rf_channel_freq(ch), 0);
    if (k_method[method].agc_off) phy_disable_agc();
    if (k_method[method].rfagc_off) phy_rfagc_disable();
    if (k_method[method].force) rf_force_gain(BENCH_GAIN);
    phy_end();
    if (k_method[method].restore) (void)rf_set_filter(rf_filter());
}

/* Each step of direct+gain, and what reading one window costs the CPU. */
static void time_steps(bench_t *b)
{
    if (rf_set_channel(DIRECT_BASE) != ESP_OK) b->failed++;
    for (unsigned i = 0; i < STEP_SAMPLES; i++) {
        int64_t t[STEPS + 1];
        phy_begin();
        t[S_FREQ] = esp_timer_get_time();
        phy_set_freq(rf_channel_freq(i % CHANNELS), 0);
        t[S_AGC] = esp_timer_get_time();
        phy_disable_agc();
        t[S_RFAGC] = esp_timer_get_time();
        phy_rfagc_disable();
        t[S_FORCE] = esp_timer_get_time();
        rf_force_gain(BENCH_GAIN);
        t[S_COPY] = esp_timer_get_time();
        phy_end();
        t[S_COPY] = esp_timer_get_time();
        (void)copy_control_window(b->window, CONTROL_BYTES);
        t[S_POWER] = esp_timer_get_time();
        volatile uint32_t power = window_power(b->window, CONTROL_BYTES);   /* kept: else optimised out */
        (void)power;
        t[STEPS] = esp_timer_get_time();
        for (unsigned s = 0; s < STEPS; s++) b->step_us[s][i] = (uint32_t)(t[s + 1] - t[s]);
        vTaskDelay(pdMS_TO_TICKS(FAR_SETTLE_MS));
    }
}

/* Power every TRACE_STEP_US from t0. A window is the newest completed descriptor,
 * so its time reads up to about 0.2 ms late: settling is an upper bound. */
static size_t trace(bench_t *b, int64_t t0)
{
    size_t n = 0;
    for (unsigned k = 1; k <= TRACE_POINTS; k++) {
        const int64_t at = t0 + (int64_t)k * TRACE_STEP_US;
        while (esp_timer_get_time() < at) {}
        if (!copy_control_window(b->window, CONTROL_BYTES)) continue;
        b->t_us[n] = (uint32_t)(esp_timer_get_time() - t0);
        b->power[n++] = window_power(b->window, CONTROL_BYTES);
    }
    return n;
}

static void run_method(bench_t *b, unsigned m)
{
    if (m != M_FULL && rf_set_channel(DIRECT_BASE) != ESP_OK) b->failed++;
    for (unsigned sweep = 0; sweep < SWEEPS; sweep++)
        for (unsigned ch = 0; ch < CHANNELS; ch++) {
            hop(b, m, (ch + CHANNELS / 2u) % CHANNELS);    /* come from across the band */
            vTaskDelay(pdMS_TO_TICKS(FAR_SETTLE_MS));
            const int64_t t0 = esp_timer_get_time();
            hop(b, m, ch);
            const int64_t t1 = esp_timer_get_time();
            const size_t n = trace(b, t1);
            b->hop_us[m][ch][sweep] = (uint32_t)(t1 - t0);
            b->settle_us[m][ch][sweep] = hs_settle_us(b->t_us, b->power, n, TAIL_POINTS, &b->level[m][ch]);
            b->clip_pm[m][ch] = measure_window(b->window, CONTROL_BYTES).clip_pm;
            if (sweep + 1u == SWEEPS && ch == hs_strongest(b->level[m], CHANNELS)) {
                const size_t kept = n < DUMP_POINTS ? n : DUMP_POINTS;   /* the VTX channel so far */
                for (size_t i = 0; i < kept; i++) {
                    b->dump_t[m][i] = b->t_us[i];
                    b->dump_p[m][i] = b->power[i];
                }
                b->dump_ch[m] = (uint8_t)ch;
                b->dump_n[m] = (uint8_t)kept;
            }
            vTaskDelay(pdMS_TO_TICKS(LATE_MS));            /* also lets idle and priority 1 run */
            if (copy_control_window(b->window, CONTROL_BYTES))
                b->late[m][ch] = window_power(b->window, CONTROL_BYTES);
        }
}

static float power_db(uint32_t p) { return p ? 10.0f * log10f((float)p / 256.0f) : -99.0f; }

/* Settled hops of s[], moved to its front, with how many were not. */
static size_t settled(uint32_t *s, size_t n, unsigned *unsettled)
{
    size_t kept = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] != HS_UNSETTLED) s[kept++] = s[i];
    *unsettled += (unsigned)(n - kept);
    return kept;
}

static void report_steps(bench_t *b)
{
    uint32_t med[STEPS];
    for (unsigned s = 0; s < STEPS; s++) {
        const hs_spread_t sp = hs_spread(b->step_us[s], STEP_SAMPLES);
        med[s] = sp.med;
        printf("HOPBENCH step %s_us=%lu/%lu/%lu\n", k_step[s], (unsigned long)sp.min, (unsigned long)sp.med,
               (unsigned long)sp.max);
    }
    b->cpu_us = med[S_COPY] + med[S_POWER];
}

static void report_trace(const bench_t *b, unsigned m)
{
    char line[512];
    int o = snprintf(line, sizeof line, "HOPBENCH trace %s R%u us:db", k_method[m].name, b->dump_ch[m] + 1u);
    for (unsigned i = 0; i < b->dump_n[m] && o < (int)sizeof line - 16; i++)
        o += snprintf(line + o, sizeof line - o, " %lu:%.1f", (unsigned long)b->dump_t[m][i], power_db(b->dump_p[m][i]));
    puts(line);
}

static void report(bench_t *b, unsigned m)
{
    uint32_t hop_med[CHANNELS], settle_med[CHANNELS], others[CHANNELS - 1u];
    unsigned kept_of[CHANNELS];
    for (unsigned ch = 0; ch < CHANNELS; ch++) {
        unsigned unsettled = 0;
        const hs_spread_t hop_s = hs_spread(b->hop_us[m][ch], SWEEPS);
        const size_t kept = settled(b->settle_us[m][ch], SWEEPS, &unsettled);
        const hs_spread_t set_s = hs_spread(b->settle_us[m][ch], kept);
        hop_med[ch] = hop_s.med;
        settle_med[ch] = set_s.med;
        kept_of[ch] = (unsigned)kept;
        printf("HOPBENCH %s R%u %u MHz hop_us=%lu/%lu/%lu settle_us=%lu/%lu/%lu settled=%u/%u "
               "power_db=%.1f late_db=%.1f clip_pm=%d\n",
               k_method[m].name, ch + 1u, rf_channel_freq(ch),
               (unsigned long)hop_s.min, (unsigned long)hop_s.med, (unsigned long)hop_s.max,
               (unsigned long)set_s.min, (unsigned long)set_s.med, (unsigned long)set_s.max,
               (unsigned)kept, SWEEPS, power_db(b->level[m][ch]), power_db(b->late[m][ch]), b->clip_pm[m][ch]);
    }
    /* The VTX's channel: noise alone says nothing about settling. */
    const size_t top = hs_strongest(b->level[m], CHANNELS);
    size_t n = 0;
    for (unsigned ch = 0; ch < CHANNELS; ch++)
        if (ch != top) others[n++] = b->level[m][ch];
    const uint32_t floor = hs_spread(others, n).med;
    const uint32_t hz_x10 = hs_rate_x10(hop_med[top], settle_med[top], WINDOW_US + b->cpu_us, CHANNELS);
    printf("HOPBENCH %s strongest=R%u hop_us=%lu settle_us=%lu settled=%u/%u contrast_db=%.1f clip_pm=%d "
           "per_channel_hz=%lu.%lu\n",
           k_method[m].name, (unsigned)top + 1u, (unsigned long)hop_med[top], (unsigned long)settle_med[top],
           kept_of[top], SWEEPS, power_db(b->level[m][top]) - power_db(floor), b->clip_pm[m][top],
           (unsigned long)(hz_x10 / 10u), (unsigned long)(hz_x10 % 10u));
    report_trace(b, m);
}

static void bench_task(void *arg)
{
    bench_t *b = arg;
    if (receiver_pause("HOPBENCH")) {
        const size_t original = rf_channel_index();
        const int offset_khz = rf_offset_khz();
        const uint8_t gain = s_current_gain;
        s_scan_active = true;
        receiver_set_gain(BENCH_GAIN);
        time_steps(b);
        for (unsigned m = 0; m < METHODS; m++) run_method(b, m);

        /* Back by the far end of the band: esp_wifi_set_channel() may do nothing for
         * the Wi-Fi channel it believes is current, and direct hops left it stale. */
        const int f = rf_channel_freq(original);
        const size_t detour = abs(f - rf_channel_freq(0)) > abs(f - rf_channel_freq(CHANNELS - 1u)) ? 0u : CHANNELS - 1u;
        if (rf_set_channel(detour) != ESP_OK || rf_set_channel(original) != ESP_OK) b->failed++;
        rf_set_offset_khz(offset_khz);
        receiver_set_gain(gain);
        s_scan_active = false;
        receiver_resume();
        report_steps(b);
        for (unsigned m = 0; m < METHODS; m++) report(b, m);
        printf("HOPBENCH done gain=%u window_us=%lu failed_retunes=%u restored=%s offset_khz=%d; "
               "reset the board before any other measurement\n", BENCH_GAIN, (unsigned long)WINDOW_US, b->failed,
               rf_channel()->name, rf_offset_khz());
    }
    free(b);
    s_running = false;
    vTaskDelete(NULL);
}

void hop_bench_start(void)
{
    if (s_running || s_menu_active || s_scan_active || menu_status_active() || calibration_in_startup() ||
        receiver_paused() || !receiver_active()) {
        printf("HOPBENCH refused: the receiver is busy\n");
        return;
    }
    bench_t *b = calloc(1, sizeof *b);
    if (!b) {
        printf("HOPBENCH refused: no memory\n");
        return;
    }
    s_running = true;
    printf("HOPBENCH start: R1-R8, %u sweeps, no detection for about 15 s\n", SWEEPS);
    if (xTaskCreate(bench_task, "hop_bench", BENCH_STACK, b, BENCH_PRIORITY, NULL) != pdPASS) {
        printf("HOPBENCH refused: no memory for the task\n");
        free(b);
        s_running = false;
    }
}
