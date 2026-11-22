/* The scan task: the one task that owns the sdr_rx engine. It calibrates every LO at boot, then
 * steps the engine hop by hop (one LO, two channels) and feeds each hop's readings to the scan
 * detector (sd_feed_hop: the pair guard holds a dominated reading of a saturated capture);
 * a pass prints a PASS line and goes over the link. Requests from the console and the link
 * (detector settings, gain, recalibrations, benches) are applied here, between two hops, so the
 * engine and the detector stay single-threaded. Priority 1, like the link task: they time-slice
 * at every tick. Every loop blocks one tick every 100 ms (cp_yield), and the task watchdog
 * watches it, so a hung gate reboots instead of reporting its last levels as live. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "checkpoint.h"
#include "cp_log.h"
#include "checkpoint_link.h"
#include "cp_bench.h"
#include "cp_internal.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "sdr_rx_bench.h"

_Static_assert(SD_FLAG_SATURATED == SDR_RD_SATURATED, "sd_feed takes the engine's reading flags as they are");
_Static_assert(SD_LEVEL_NONE == SDR_LEVEL_NONE, "a missing reading is the same code on both sides");
_Static_assert(PD_CHANNELS == SDR_RX_CHANNELS, "one detector slot per channel");
_Static_assert(SD_HOP_READINGS == SDR_RX_PER_HOP, "sd_feed_hop takes every reading of a hop");
_Static_assert(SD_HOPS_MAX >= SDR_RX_MAX_HOPS, "one saturated-run record per plan hop");

#if CONFIG_CHECKPOINT_PAIR_GUARD
#define CP_PAIR_GUARD 1u
#else
#define CP_PAIR_GUARD 0u
#endif

bool cp_wdt_subscribed;

static StackType_t *s_stack;         /* reserved at boot (checkpoint_reserve) */
static StaticTask_t *s_tcb;
static TaskHandle_t s_task;
static int64_t s_boot_t0_us;
static uint16_t s_ch_mhz[SDR_RX_CHANNELS];

bool cp_scan_reserve(void)
{
    if (s_stack && s_tcb) return true;
    s_stack = heap_caps_malloc(CP_SCAN_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_tcb = heap_caps_calloc(1, sizeof(*s_tcb), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_stack && s_tcb) return true;
    free(s_stack);
    free(s_tcb);
    s_stack = NULL;
    s_tcb = NULL;
    return false;
}

TaskHandle_t cp_scan_task(void) { return s_task; }

void cp_on_pass(cp_t *cp, const pd_event_t *e)
{
    ++cp->passes;
    cp->last = *e;
    cp->has_last = true;
    /* peak_ms (console and wire) is the smoothed peak; comp_ms and est_peak_ms estimate the true
     * one, for the console only (the fleet times every gate on the same smoothed peak). */
    const uint32_t comp_ms = (uint32_t)lroundf(sd_delay_ms(&cp->sd, e->channel));
    /* held: the channel's readings the pair guard held since its previous pass (or the boot). */
    const unsigned ch = e->channel % SDR_RX_CHANNELS;
    const uint32_t held = cp->sd.held[ch] - cp->held_at_pass[ch];
    cp->held_at_pass[ch] = cp->sd.held[ch];
    /* rearm: how the channel was armed for it, the release (abs) or the relative re-arm (rel). */
    cp_printf("PASS channel=R%u freq=%u peak_db=%.1f peak_ms=%" PRIu32 " at_ms=%" PRIu32 " delay_ms=%" PRIu32
           " comp_ms=%" PRIu32 " est_peak_ms=%" PRIu32 " tau_ms=%.0f held=%" PRIu32 " rearm=%s\n",
           e->channel + 1u, (unsigned)s_ch_mhz[ch], (double)e->peak_db, e->peak_ms, e->at_ms, e->at_ms - e->peak_ms,
           comp_ms, e->peak_ms - comp_ms, (double)sd_tau_ms(&cp->sd), held, e->rel ? "rel" : "abs");
    checkpoint_link_report_pass(e);
}

/* K l: the rises, then each plan hop's saturated captures since the previous line (sat=, hop order:
 * a capture that hit an ADC rail, the flag the detector sees on both of its readings), then each
 * channel's readings the pair guard held since the previous line (held=, R1..R8). */
static void cp_trace(cp_t *cp, int64_t now_us)
{
    const scan_detector_t *sd = &cp->sd;
    const unsigned top = sd_strongest(sd);
    const sdr_plan_t *p = sdr_bench_plan();
    const unsigned nhops = p && p->nhops < SDR_RX_MAX_HOPS ? p->nhops : SDR_RX_MAX_HOPS;
    cp_printf("CP_TRACE t_ms=%lld rise_db=%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f top=R%u state=%s sat=", now_us / 1000,
           (double)sd_rise_db(sd, 0), (double)sd_rise_db(sd, 1), (double)sd_rise_db(sd, 2),
           (double)sd_rise_db(sd, 3), (double)sd_rise_db(sd, 4), (double)sd_rise_db(sd, 5),
           (double)sd_rise_db(sd, 6), (double)sd_rise_db(sd, 7), top + 1u, pd_state_name(sd->det.ch[top].state));
    for (unsigned j = 0; j < nhops; j++) cp_printf("%s%u", j ? "," : "", (unsigned)cp->trace_sat[j]);
    cp_printf(" held=");
    for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) {
        cp_printf("%s%" PRIu32, ch ? "," : "", sd->held[ch] - cp->held_at_trace[ch]);
        cp->held_at_trace[ch] = sd->held[ch];
    }
    cp_printf("\n");
    memset(cp->trace_sat, 0, sizeof(cp->trace_sat));
}

unsigned cp_feed_hop(scan_detector_t *sd, const sdr_hop_t *h, pd_event_t ev[SDR_RX_PER_HOP], unsigned *held)
{
    unsigned rang = 0;
    *held = 0;
#if CP_PAIR_GUARD
    sd_reading_t r[SD_HOP_READINGS];
    const unsigned n = h->n < SD_HOP_READINGS ? h->n : SD_HOP_READINGS;
    for (unsigned i = 0; i < n; i++)
        r[i] = (sd_reading_t){.t_us = h->r[i].t_us, .level_qdb = h->r[i].level_qdb, .channel = h->r[i].channel,
                              .flags = h->r[i].flags};
    rang = sd_feed_hop(sd, h->hop, r, n, ev, held);
#else
    for (unsigned i = 0; i < h->n && i < SDR_RX_PER_HOP; i++)
        if (sd_feed(sd, h->r[i].channel, h->r[i].level_qdb, h->r[i].flags, h->r[i].t_us, &ev[i])) rang |= 1u << i;
#endif
    return rang;
}

/* A bench in this task (the console posted it), then the production settings again. */
static void run_bench(cp_t *cp, char key, cp_yield_t *y)
{
    if (sdr_rx_calibrating()) {                  /* a recalibration got in first */
        cp_printf("CHECKPOINT bench=%c refused=busy\n", key);
        return;
    }
    cp->scan_stop = false;
    cp_bench_run(cp, key, y);
    sdr_bench_set_tune_method(SDR_TUNE_PLL);
    sdr_bench_set_plan(SDR_PLAN_PAIRS);          /* also resets the IQ states */
    sd_relearn(&cp->sd);
    cp_mark_read(cp, cp_now_ms());
}

/* The mailbox (cp_internal.h): detector settings, gain (+ relearn), a recalibration, a bench. */
static void cp_apply_requests(cp_t *cp, cp_yield_t *y)
{
    cp_calib_apply(cp, esp_timer_get_time());
    if (cp->wanted_dirty) {
        portENTER_CRITICAL(&cp_mux);
        const pd_config_t c = cp->wanted;
        const float rearm = cp->wanted_rearm_db;
        cp->wanted_dirty = false;
        portEXIT_CRITICAL(&cp_mux);
        (void)sd_set_config(&cp->sd, &c);        /* validated by the poster */
        pd_set_rearm(&cp->sd.det, rearm);        /* from the next reading; 0 sends a relatively armed channel to COOLDOWN */
    }
    if (cp->gain_dirty) {
        portENTER_CRITICAL(&cp_mux);
        const uint8_t gain = cp->gain;
        cp->gain_dirty = false;
        portEXIT_CRITICAL(&cp_mux);
        sdr_rx_request_gain(gain);               /* applied by the next sdr_rx_step */
        sd_relearn(&cp->sd);
    }
    if (cp->recal_req >= 0) {
        portENTER_CRITICAL(&cp_mux);
        const int kind = cp->recal_req;
        const char *from = cp->recal_from;
        cp->recal_req = -1;
        portEXIT_CRITICAL(&cp_mux);
        if (sdr_rx_request_recal((sdr_recal_t)kind)) {
            cp->relearn_pending = true;          /* sd_relearn once it has run */
        } else {
            cp->relearn_pending = false;         /* a later foreign recalibration must not relearn */
            cp_printf("CHECKPOINT command=%s refused=busy from=%s\n", kind == SDR_RECAL_FULL ? "cal_full" : "cal_quick",
                   from ? from : "?");
        }
    }
    if (cp->bench_req) {
        portENTER_CRITICAL(&cp_mux);
        const char key = (char)cp->bench_req;
        cp->bench_req = 0;
        cp->bench_key = (uint8_t)key;
        cp->bench_running = true;
        portEXIT_CRITICAL(&cp_mux);
        run_bench(cp, key, y);
        portENTER_CRITICAL(&cp_mux);
        cp->bench_running = false;
        cp->bench_key = 0;
        portEXIT_CRITICAL(&cp_mux);
    }
}

static void print_ready(const cp_t *cp, uint32_t cal_ms)
{
    sdr_rx_health_t h;
    sdr_rx_get_health(&h);
    char lo[SDR_RX_MAX_HOPS * 6 + 1] = "";
    size_t len = 0;
    for (unsigned j = 0; j < h.nhops && j < SDR_RX_MAX_HOPS && len + 6 < sizeof(lo); j++)
        len += (size_t)snprintf(lo + len, sizeof(lo) - len, "%s%u", j ? "," : "", (unsigned)h.lo_mhz[j]);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    portENTER_CRITICAL(&cp_mux);
    const float rearm = cp->wanted_rearm_db;
    portEXIT_CRITICAL(&cp_mux);
    /* tools/flash.py waits for this line (BOOT_DONE). verify_tries and pair_guard show the build's
     * settings: an sdkconfig kept from a bench control (verify_tries=1: the verify only logs;
     * pair_guard=0: CONFIG_CHECKPOINT_PAIR_GUARD=n) is visible in every log. rearm_db is the relative
     * re-arm's drop the detector starts with (saved; the build's on a first boot; 0: none). */
    cp_printf("SDR ready lo=%s gain=%u verify_tries=%u pair_guard=%u rearm_db=%.1f cal_ms=%" PRIu32
           " boot_ms=%lld chip_rev=v%u.%u\n",
           lo, (unsigned)sdr_rx_gain(), (unsigned)CONFIG_SDR_RX_VERIFY_TRIES, CP_PAIR_GUARD, (double)rearm, cal_ms,
           (esp_timer_get_time() - s_boot_t0_us) / 1000, (unsigned)(chip.revision / 100u),
           (unsigned)(chip.revision % 100u));
}

static void scan_task(void *arg)
{
    cp_t *cp = arg;
    sdr_rx_config_t rc = sdr_rx_config_default();
    for (unsigned j = 0; j < SDR_RX_CHANNELS; j++) s_ch_mhz[j] = (uint16_t)(rc.ch_khz[j] / 1000u);
    portENTER_CRITICAL(&cp_mux);
    rc.gain = cp->gain;                          /* the init applies it */
    cp->gain_dirty = false;
    portEXIT_CRITICAL(&cp_mux);
    checkpoint_boot_stage("sdr_init");
    const int64_t t0 = esp_timer_get_time();
    if (sdr_rx_init(&rc) != ESP_OK) {            /* not subscribed to the TWDT: a suspended task would trip it */
        cp->engine_failed = true;                /* STATS: CAL_NEEDED, faults >= 1, levels unknown */
        checkpoint_boot_stage("sdr_failed");
        cp_printf("SDR init failed: no detection; console stays up\n");
        for (;;) vTaskSuspend(NULL);
    }
    print_ready(cp, (uint32_t)((esp_timer_get_time() - t0) / 1000));
    checkpoint_boot_stage("ready");
    if (esp_task_wdt_add(NULL) == ESP_OK) cp_wdt_subscribed = true;   /* a hung scan task reboots the gate */
    else cp_printf("CHECKPOINT warning: the scan task is not watched by the task watchdog\n");
    portENTER_CRITICAL(&cp_mux);
    const pd_config_t cfg = cp->wanted;
    const float rearm = cp->wanted_rearm_db;
    cp->wanted_dirty = false;
    portEXIT_CRITICAL(&cp_mux);
    sd_init(&cp->sd, &cfg);
    pd_set_rearm(&cp->sd.det, rearm);            /* sd_init turns it off */
    cp_mark_read(cp, cp_now_ms());
    cp->live = true;
    cp_yield_t y = {.last_us = esp_timer_get_time()};
    for (;;) {
        cp_apply_requests(cp, &y);               /* settings, gain, recalibration, bench */
        sdr_hop_t h;
        sdr_rx_step(&h);                         /* applies pending recal/gain itself */
        if (h.recalibrated) {
            cp_mark_read(cp, cp_now_ms());       /* the end of a calibration is not a stall */
            if (cp->relearn_pending) {
                sd_relearn(&cp->sd);
                cp->relearn_pending = false;
            }
        }
        if (cp->trace && h.n && (h.r[0].flags & SDR_RD_SATURATED) && h.hop < SDR_RX_MAX_HOPS &&
            cp->trace_sat[h.hop] < UINT16_MAX)
            cp->trace_sat[h.hop]++;              /* one flag per capture, on both of its readings */
        if (h.n) cp_calib_capture(cp, h.r[0].flags & SDR_RD_SATURATED);
        pd_event_t ev[SDR_RX_PER_HOP];
        unsigned held;
        const unsigned rang = cp_feed_hop(&cp->sd, &h, ev, &held);
        for (unsigned i = 0; i < h.n; i++) {
            cp->readings++;
            /* A held reading is none: a channel held CP_STALL_MS reads unknown in STATS. */
            if (!(held & (1u << i))) cp->last_reading_ms[h.r[i].channel % SDR_RX_CHANNELS] = (uint32_t)(h.r[i].t_us / 1000);
            if (rang & (1u << i)) cp_on_pass(cp, &ev[i]);
        }
        const int64_t now = esp_timer_get_time();
        if (h.rotation_end) {
            sd_rotation(&cp->sd, now);
            cp_calib_rotation(cp, now);
            cp->rotations++;
            cp->last_rotation_ms = (uint32_t)(now / 1000);
            if (cp_wdt_subscribed) (void)esp_task_wdt_reset();
            if (cp->trace && now - cp->last_trace_us >= CP_TRACE_US) {
                cp_trace(cp, now);
                cp->last_trace_us = now;
            }
        }
        cp->learning = sd_learning(&cp->sd, now);
        cp_yield(&y);
    }
}

bool cp_scan_start(cp_t *cp, int64_t boot_t0_us)
{
    if (s_task) return true;
    if (!cp_scan_reserve()) return false;
    s_boot_t0_us = boot_t0_us;
    s_task = xTaskCreateStatic(scan_task, "checkpoint", CP_SCAN_STACK, cp, CP_SCAN_PRIORITY, s_stack, s_tcb);
    return s_task != NULL;
}
