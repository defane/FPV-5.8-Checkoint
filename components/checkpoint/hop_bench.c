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
 * The scan recording (K then S) loops the `force` hop over R1-R8 and records the
 * power of every channel, to see whether real passes stand out and for how long
 * it holds, and the residue trace (K then z) times what a strong channel leaves in
 * the next reading. Both at the end of the file.
 * Each takes the receiver from the detector's own scan (scan_hop.c, checkpoint.c),
 * then gives it back. No detection meanwhile, and nothing is saved. The protected
 * receive chain is not edited: only its public calls and, for the direct hops, the
 * same libphy functions rf.c uses. A one-shot priority-1 task with its buffers on
 * the heap, gone when the bench ends. */
#include "hop_bench.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "hop_stats.h"
#include "phy.h"
#include "receiver.h"
#include "rf.h"
#include "scan_hop.h"
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
#define CHANNELS        SCAN_CHANNELS
#define DIRECT_BASE     SCAN_BASE_CHANNEL
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

static volatile bool s_running, s_scanning, s_scan_stop;
static uint8_t s_hop_gain = BENCH_GAIN;   /* forced again after each direct hop */

static bool take_receiver(const char *tag, uint8_t gain, scan_saved_t *saved)
{
    if (!scan_take(tag, gain, saved)) return false;
    s_hop_gain = gain;
    return true;
}

/* The detector's scan lets go of the receiver for as long as a bench runs. */
static bool bench_claim(void)
{
    checkpoint_scan_hold(true);
    if (!s_running && scan_receiver_free()) return true;
    checkpoint_scan_hold(false);
    return false;
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
    if (k_method[method].force) rf_force_gain(s_hop_gain);
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
        rf_force_gain(s_hop_gain);
        t[S_COPY] = esp_timer_get_time();
        phy_end();
        t[S_COPY] = esp_timer_get_time();
        (void)copy_control_window(b->window, CONTROL_BYTES);
        t[S_POWER] = esp_timer_get_time();
        volatile uint32_t power = hs_window_power(b->window, CONTROL_BYTES);   /* kept: else optimised out */
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
        b->power[n++] = hs_window_power(b->window, CONTROL_BYTES);
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
                b->late[m][ch] = hs_window_power(b->window, CONTROL_BYTES);
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
    scan_saved_t saved;
    if (take_receiver("HOPBENCH", BENCH_GAIN, &saved)) {
        time_steps(b);
        for (unsigned m = 0; m < METHODS; m++) run_method(b, m);
        b->failed += scan_give(&saved);
        report_steps(b);
        for (unsigned m = 0; m < METHODS; m++) report(b, m);
        printf("HOPBENCH done gain=%u window_us=%lu failed_retunes=%u restored=%s offset_khz=%d; "
               "reset the board before any other measurement\n", BENCH_GAIN, (unsigned long)WINDOW_US, b->failed,
               rf_channel()->name, rf_offset_khz());
    }
    free(b);
    s_running = false;
    checkpoint_scan_hold(false);
    vTaskDelete(NULL);
}

void hop_bench_start(void)
{
    if (!bench_claim()) {
        printf("HOPBENCH refused: the receiver is busy\n");
        return;
    }
    bench_t *b = calloc(1, sizeof *b);
    if (!b) {
        printf("HOPBENCH refused: no memory\n");
        checkpoint_scan_hold(false);
        return;
    }
    s_running = true;
    printf("HOPBENCH start: R1-R8, %u sweeps, no detection for about 15 s\n", SWEEPS);
    if (xTaskCreate(bench_task, "hop_bench", BENCH_STACK, b, BENCH_PRIORITY, NULL) != pdPASS) {
        printf("HOPBENCH refused: no memory for the task\n");
        free(b);
        s_running = false;
        checkpoint_scan_hold(false);
    }
}

/* ---- Scan recording (console K then S) -------------------------------------
 * Hops over R1-R8 with `force` until K then S again (SCAN_MAX_S at most) and keeps
 * the last SCAN_KEEP_S seconds of every channel's power in a heap ring: printing
 * while hopping would slow the scan, so the ring is printed at the end, as
 * "SCAN,t_us,p1..p8" lines for tools/scan_analyze.py. A SCANBENCH live line every
 * SCAN_LIVE_S shows it is still running, for endurance.
 * K then g/G sets its gain: a close VTX saturates the 4-bit IQ at the hop bench's 52,
 * and at 40 the other channels fill with bursts; a pass must rise out of the noise
 * without reaching the rail. The hops cross
 * the band (scan_order): the channel read after another is never its neighbour in
 * frequency, so a residue of the previous channel and a real bleed fall on different
 * channels, and each hop is the 4-channel step the hop bench measured. */
#define SCAN_MAX_S        3600u
#define SCAN_KEEP_S       60u           /* less on a tight heap: SCANBENCH start tells */
#define SCAN_HZ           57u           /* hop + SCAN_FRESH_US + one window, over 8 channels */
#define SCAN_LIVE_S       10u
#define SCAN_HEADROOM     16384u        /* heap left for the rest of the firmware */
#define SCAN_MIN_SWEEPS   500u
#define SCAN_DUMP_PAUSE_MS 200u
#define SCAN_DUMP_LINES   4u
#define SCAN_DUMP_MS      5u            /* a full ring prints in about 5 s */

typedef struct {
    uint32_t t_us;
    uint16_t power[CHANNELS];           /* hs_window_power(); fits: at most 32768 */
} sweep_t;

typedef struct {
    uint8_t window[CONTROL_BYTES];
    size_t cap;
    uint32_t count, no_data;
    sweep_t ring[];
} scan_t;

static void scan_loop(scan_t *sc)
{
    uint16_t peak[CHANNELS] = {0};
    const int64_t t0 = esp_timer_get_time();
    int64_t next_live = t0 + SCAN_LIVE_S * 1000000ll;
    uint32_t live_from = 0;
    while (!s_scan_stop) {
        const int64_t now = esp_timer_get_time();
        if (now - t0 >= SCAN_MAX_S * 1000000ll) break;
        sweep_t *e = &sc->ring[sc->count % sc->cap];
        e->t_us = (uint32_t)(now - t0);
        scan_sweep(sc->window, e->power, &sc->no_data);
        sc->count++;
        for (unsigned ch = 0; ch < CHANNELS; ch++)
            if (e->power[ch] != SD_NO_DATA && e->power[ch] > peak[ch]) peak[ch] = e->power[ch];
        if (sc->count % SCAN_YIELD_SWEEPS == 0u) vTaskDelay(1);
        if (esp_timer_get_time() >= next_live) {
            const uint32_t sweeps = sc->count - live_from;
            printf("SCANBENCH live t_s=%lu sweeps=%lu per_channel_hz=%lu no_data=%lu peak_db=%.0f/%.0f/%.0f/%.0f/%.0f/%.0f/%.0f/%.0f\n",
                   (unsigned long)((esp_timer_get_time() - t0) / 1000000), (unsigned long)sc->count,
                   (unsigned long)(sweeps / SCAN_LIVE_S), (unsigned long)sc->no_data,
                   power_db(peak[0]), power_db(peak[1]), power_db(peak[2]), power_db(peak[3]),
                   power_db(peak[4]), power_db(peak[5]), power_db(peak[6]), power_db(peak[7]));
            for (unsigned ch = 0; ch < CHANNELS; ch++) peak[ch] = 0;
            live_from = sc->count;
            next_live += SCAN_LIVE_S * 1000000ll;
        }
    }
}

/* stdout never blocks (main.c): what overflows the USB FIFO is dropped, so the
 * burst is paced, after a pause for the lines the receiver prints as it resumes. */
static void scan_dump(const scan_t *sc)
{
    vTaskDelay(pdMS_TO_TICKS(SCAN_DUMP_PAUSE_MS));
    const size_t kept = hs_ring_kept(sc->count, sc->cap), oldest = hs_ring_oldest(sc->count, sc->cap);
    char order[2u * CHANNELS + 1u];
    for (unsigned k = 0; k < CHANNELS; k++) {
        order[2u * k] = (char)('1' + scan_order[k]);
        order[2u * k + 1u] = k + 1u < CHANNELS ? ':' : '\0';
    }
    printf("SCANDUMP begin sweeps=%lu kept=%u no_data=%lu gain=%u order=%s\n", (unsigned long)sc->count,
           (unsigned)kept, (unsigned long)sc->no_data, scan_gain(), order);
    for (size_t i = 0; i < kept; i++) {
        const sweep_t *e = &sc->ring[(oldest + i) % sc->cap];
        printf("SCAN,%lu,%u,%u,%u,%u,%u,%u,%u,%u\n", (unsigned long)e->t_us, e->power[0], e->power[1], e->power[2],
               e->power[3], e->power[4], e->power[5], e->power[6], e->power[7]);
        if (i % SCAN_DUMP_LINES == 0u) vTaskDelay(pdMS_TO_TICKS(SCAN_DUMP_MS));
    }
    printf("SCANDUMP end gain=%u order=%s\n", scan_gain(), order);   /* the begin line can get lost */
}

static void scan_task(void *arg)
{
    scan_t *sc = arg;
    scan_saved_t saved;
    if (take_receiver("SCANBENCH", scan_gain(), &saved)) {
        if (rf_set_channel(DIRECT_BASE) == ESP_OK) scan_loop(sc);
        const unsigned failed = scan_give(&saved);
        scan_dump(sc);
        printf("SCANBENCH done failed_retunes=%u restored=%s; reset the board before any other measurement\n",
               failed, rf_channel()->name);
    }
    free(sc);
    s_running = s_scanning = false;
    checkpoint_scan_hold(false);
    vTaskDelete(NULL);
}

void scan_bench_gain(bool up)
{
    if (s_scanning) {
        printf("SCANBENCH gain refused: a scan is running\n");
        return;
    }
    scan_gain_step(up);
    printf("SCANBENCH gain=%u, now and for the next recording (not saved)\n", scan_gain());
}

void scan_bench_toggle(void)
{
    if (s_scanning) {
        s_scan_stop = true;
        printf("SCANBENCH stopping\n");
        return;
    }
    if (!bench_claim()) {
        printf("SCANBENCH refused: the receiver is busy\n");
        return;
    }
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t room = largest > SCAN_HEADROOM + sizeof(scan_t) ? largest - SCAN_HEADROOM - sizeof(scan_t) : 0u;
    size_t cap = (size_t)SCAN_KEEP_S * SCAN_HZ;
    if (cap > room / sizeof(sweep_t)) cap = room / sizeof(sweep_t);
    scan_t *sc = cap >= SCAN_MIN_SWEEPS ? calloc(1, sizeof *sc + cap * sizeof(sweep_t)) : NULL;
    if (!sc) {
        printf("SCANBENCH refused: no memory (largest block %u)\n", (unsigned)largest);
        checkpoint_scan_hold(false);
        return;
    }
    sc->cap = cap;
    s_scan_stop = false;
    s_running = s_scanning = true;
    printf("SCANBENCH start: R1-R8 gain=%u keeping the last %u s; K then S stops, %u s at most; no detection meanwhile\n",
           scan_gain(), (unsigned)(cap / SCAN_HZ), SCAN_MAX_S);
    if (xTaskCreate(scan_task, "scan_bench", BENCH_STACK, sc, BENCH_PRIORITY, NULL) != pdPASS) {
        printf("SCANBENCH refused: no memory for the task\n");
        free(sc);
        s_running = s_scanning = false;
        checkpoint_scan_hold(false);
    }
}

/* ---- Residue trace (console K then z) ---------------------------------------
 * After a hop off a strong channel, the next channel's first window still shows
 * that signal about 8 dB down (scan recording, board 2026-11-05: it stayed with the
 * channel read next, not with the neighbour in frequency). This times it: strongest
 * channel to the quietest, a delay, then the IQ power of the four quarters of one
 * window (25 us each), against the same hop from another quiet channel. The delay
 * where both agree is how long a scan must wait, and the quarters which samples to
 * skip. At the scan's gain, with a VTX close enough to read well above the noise. */
#define RES_REPEATS     8u
#define RES_QUARTERS    4u
#define RES_SETTLE_MS   3u
#define RES_LEVEL_READS 4u

static const uint16_t k_res_delay_us[] = {0, 50, 100, 150, 200, 250, 300, 400, 500, 600, 800, 1000, 1500, 2000, 3000, 5000};
#define RES_DELAYS (sizeof k_res_delay_us / sizeof k_res_delay_us[0])

typedef struct {
    uint8_t window[CONTROL_BYTES];
    uint32_t level[CHANNELS];
    uint32_t after[RES_DELAYS][RES_QUARTERS], control[RES_DELAYS][RES_QUARTERS];
    uint32_t after_n[RES_DELAYS], control_n[RES_DELAYS];
} residue_t;

static void residue_levels(residue_t *r)
{
    for (unsigned ch = 0; ch < CHANNELS; ch++) {
        hop(NULL, M_FORCE, ch);
        vTaskDelay(pdMS_TO_TICKS(RES_SETTLE_MS));
        uint32_t sum = 0, n = 0;
        for (unsigned k = 0; k < RES_LEVEL_READS; k++) {
            if (copy_control_window(r->window, CONTROL_BYTES)) {
                sum += hs_window_power(r->window, CONTROL_BYTES);
                n++;
            }
            vTaskDelay(1);
        }
        r->level[ch] = n ? sum / n : 0u;
    }
}

static void residue_measure(residue_t *r, unsigned from, unsigned to, uint32_t (*out)[RES_QUARTERS], uint32_t *count)
{
    const size_t q = CONTROL_BYTES / RES_QUARTERS;
    for (unsigned d = 0; d < RES_DELAYS; d++)
        for (unsigned rep = 0; rep < RES_REPEATS; rep++) {
            hop(NULL, M_FORCE, from);
            vTaskDelay(pdMS_TO_TICKS(RES_SETTLE_MS));
            hop(NULL, M_FORCE, to);
            const int64_t at = esp_timer_get_time() + k_res_delay_us[d];
            while (esp_timer_get_time() < at) {}
            if (!copy_control_window(r->window, CONTROL_BYTES)) continue;
            for (unsigned k = 0; k < RES_QUARTERS; k++) out[d][k] += hs_window_power(r->window + k * q, q);
            count[d]++;
        }
}

static void residue_report(const residue_t *r, unsigned top, unsigned quiet, unsigned other)
{
    printf("RESIDUE strong=R%u %.1f dB quiet=R%u %.1f dB control=R%u %.1f dB gain=%u%s\n", top + 1u,
           power_db(r->level[top]), quiet + 1u, power_db(r->level[quiet]), other + 1u, power_db(r->level[other]),
           scan_gain(), power_db(r->level[top]) < 5.0f ? " (weak: bring the VTX closer)" : "");
    for (unsigned d = 0; d < RES_DELAYS; d++) {
        const uint32_t a = r->after_n[d] ? r->after_n[d] : 1u, c = r->control_n[d] ? r->control_n[d] : 1u;
        printf("RESIDUE d_us=%u after_db=%.1f/%.1f/%.1f/%.1f control_db=%.1f/%.1f/%.1f/%.1f\n", k_res_delay_us[d],
               power_db(r->after[d][0] / a), power_db(r->after[d][1] / a), power_db(r->after[d][2] / a),
               power_db(r->after[d][3] / a), power_db(r->control[d][0] / c), power_db(r->control[d][1] / c),
               power_db(r->control[d][2] / c), power_db(r->control[d][3] / c));
    }
}

static void residue_task(void *arg)
{
    residue_t *r = arg;
    scan_saved_t saved;
    if (take_receiver("RESIDUE", scan_gain(), &saved)) {
        unsigned top = 0, quiet = 0, other = 0;
        if (rf_set_channel(DIRECT_BASE) == ESP_OK) {
            residue_levels(r);
            top = (unsigned)hs_strongest(r->level, CHANNELS);
            quiet = top ? 0u : 1u;
            for (unsigned ch = 0; ch < CHANNELS; ch++)
                if (ch != top && r->level[ch] < r->level[quiet]) quiet = ch;
            other = quiet == (top ? 0u : 1u) ? (top == 2u ? 3u : 2u) : (top ? 0u : 1u);
            for (unsigned ch = 0; ch < CHANNELS; ch++)
                if (ch != top && ch != quiet && r->level[ch] < r->level[other]) other = ch;
            residue_measure(r, top, quiet, r->after, r->after_n);
            residue_measure(r, other, quiet, r->control, r->control_n);
        }
        const unsigned failed = scan_give(&saved);
        residue_report(r, top, quiet, other);
        printf("RESIDUE done failed_retunes=%u restored=%s; reset the board before any other measurement\n",
               failed, rf_channel()->name);
    }
    free(r);
    s_running = false;
    checkpoint_scan_hold(false);
    vTaskDelete(NULL);
}

void residue_bench_start(void)
{
    if (!bench_claim()) {
        printf("RESIDUE refused: the receiver is busy\n");
        return;
    }
    residue_t *r = calloc(1, sizeof *r);
    if (!r) {
        printf("RESIDUE refused: no memory\n");
        checkpoint_scan_hold(false);
        return;
    }
    s_running = true;
    printf("RESIDUE start: VTX on and close, no detection for about 5 s\n");
    if (xTaskCreate(residue_task, "residue", BENCH_STACK, r, BENCH_PRIORITY, NULL) != pdPASS) {
        printf("RESIDUE refused: no memory for the task\n");
        free(r);
        s_running = false;
        checkpoint_scan_hold(false);
    }
}
