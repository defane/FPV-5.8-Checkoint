/* Checkpoint pass detection (checkpoint.h).
 *
 * A priority-1 task (under control/gain_obs at 3 and gain_fast at 4) owns the
 * receiver and sweeps R1-R8 all the time (scan_hop.c): nobody picks a channel, every
 * drone on Raceband is seen. Each sweep goes to components/pass_detector's scan
 * detector, and a pass prints a PASS line and goes over the UART link
 * (checkpoint_link.c). It lets go of the receiver for a calibration and for a console
 * bench (hop_bench.c), then starts over: no sample meanwhile.
 *
 * The detector has no local output: no amplifier (the coin sound is played
 * centrally by the C6 master) and no status LED (the C5's onboard WS2812 sits
 * on a receiver IQ-loopback pin). Console (console.c): K then one key per
 * change tunes the detector (checkpoint_help). Settings are kept in NVS
 * "checkpoint".
 *
 * In production nobody opens this C5's USB console, so the bridge (a C3 node,
 * or the C6 master for its own gate) gets the console's T as health in STATS
 * and its B, F and a reset as link COMMANDs. */
#include "checkpoint.h"
#include "receiver.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "checkpoint_link.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hop_bench.h"
#include "pass_detector.h"
#include "rf.h"
#include "scan_detector.h"
#include "scan_hop.h"
#include "sdkconfig.h"

#define CHECKPOINT_WAIT_MS   100u   /* the receiver is someone else's */
#define CHECKPOINT_PRIORITY  1u     /* below every receiver task */
#define CHECKPOINT_STACK     4096u
#define CHECKPOINT_HOLD_POLLS 40u   /* 10 ms each: a sweep is under 20 */
#define CHECKPOINT_TRACE_SWEEPS 12u /* 5 lines/s */

/* NVS "checkpoint"; a new layout or meaning gets a new version (old blobs ignored).
 * 2: dB of rise above a channel's rest, no longer of C/N. The three bytes after the
 * version held the volume and mute of the removed local sound: kept as padding. */
#define PREFS_VERSION 2u
typedef struct {
    uint8_t version, reserved[3];
    pd_config_t det;
} checkpoint_prefs_t;

typedef struct {
    scan_detector_t sd;
    pd_config_t wanted;              /* console copy; the task applies it */
    volatile bool wanted_dirty, trace, live;   /* live: sweeping, the levels are real */
    uint32_t sweeps, no_data, passes;
    bool has_last;
    pd_event_t last;
    uint8_t window[CONTROL_BYTES];
} checkpoint_t;

static checkpoint_t *s_cp;
static portMUX_TYPE s_wanted_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_hold;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void on_pass(checkpoint_t *cp, const pd_event_t *e)
{
    ++cp->passes;
    cp->last = *e;
    cp->has_last = true;
    printf("PASS channel=R%u freq=%u peak_db=%.1f peak_ms=%lu at_ms=%lu delay_ms=%lu\n", e->channel + 1u,
           rf_channel_freq(e->channel), e->peak_db, (unsigned long)e->peak_ms, (unsigned long)e->at_ms,
           (unsigned long)(e->at_ms - e->peak_ms));
    checkpoint_link_report_pass(e);
}

static void apply_wanted(checkpoint_t *cp)
{
    if (!cp->wanted_dirty) return;
    pd_config_t c;
    taskENTER_CRITICAL(&s_wanted_lock);
    c = cp->wanted;
    cp->wanted_dirty = false;
    taskEXIT_CRITICAL(&s_wanted_lock);
    (void)pd_set_config(&cp->sd.det, &c);   /* validated by the console */
}

static void trace_line(const checkpoint_t *cp, uint32_t now)
{
    const scan_detector_t *sd = &cp->sd;
    const unsigned top = sd_strongest(sd);
    printf("CP_TRACE t_ms=%lu rise_db=%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f top=R%u state=%s\n", (unsigned long)now,
           sd_rise_db(sd, 0), sd_rise_db(sd, 1), sd_rise_db(sd, 2), sd_rise_db(sd, 3), sd_rise_db(sd, 4),
           sd_rise_db(sd, 5), sd_rise_db(sd, 6), sd_rise_db(sd, 7), top + 1u, pd_state_name(sd->det.ch[top].state));
}

/* Sweeps until a calibration or a bench wants the receiver. */
static void scan_run(checkpoint_t *cp)
{
    const pd_config_t cfg = cp->sd.det.cfg;
    sd_init(&cp->sd, &cfg);              /* the band may have changed meanwhile */
    cp->live = true;
    while (!s_hold && !calibration_in_startup()) {
        uint16_t power[SCAN_CHANNELS];
        pd_event_t events[SCAN_CHANNELS];
        apply_wanted(cp);
        scan_sweep(cp->window, power, &cp->no_data);
        const uint32_t now = now_ms();
        const unsigned n = sd_sweep(&cp->sd, power, now, events);
        for (unsigned i = 0; i < n; i++) on_pass(cp, &events[i]);
        if (cp->trace && cp->sweeps % CHECKPOINT_TRACE_SWEEPS == 0u) trace_line(cp, now);
        if (++cp->sweeps % SCAN_YIELD_SWEEPS == 0u) vTaskDelay(1);
    }
    cp->live = false;
}

static void checkpoint_task(void *arg)
{
    checkpoint_t *cp = arg;
    for (;;) {
        apply_wanted(cp);
        scan_saved_t saved;
        if (s_hold || !scan_receiver_free() || !scan_take("CHECKPOINT", scan_gain(), &saved)) {
            vTaskDelay(pdMS_TO_TICKS(CHECKPOINT_WAIT_MS));
            continue;
        }
        if (rf_set_channel(SCAN_BASE_CHANNEL) == ESP_OK) {
            printf("CHECKPOINT scan=on R1-R8 gain=%u\n", scan_gain());
            scan_run(cp);
        } else {
            printf("CHECKPOINT scan refused=retune\n");
        }
        (void)scan_give(&saved);
        printf("CHECKPOINT scan=off\n");
    }
}

void checkpoint_scan_hold(bool hold)
{
    s_hold = hold;
    if (!hold || !s_cp) return;
    for (unsigned k = 0; k < CHECKPOINT_HOLD_POLLS && s_scan_active; k++) vTaskDelay(pdMS_TO_TICKS(10));
}

void checkpoint_start(void)
{
    if (s_cp) return;
    checkpoint_t *cp = calloc(1, sizeof(*cp));
    if (!cp) { printf("CHECKPOINT disabled: no memory\n"); return; }
    checkpoint_prefs_t prefs;
    const bool saved = blob_load("checkpoint", &prefs, sizeof(prefs)) && prefs.version == PREFS_VERSION &&
                       pd_config_valid(&prefs.det);
    const pd_config_t cfg = saved ? prefs.det : sd_config_default();
    sd_init(&cp->sd, &cfg);
    cp->wanted = cfg;
    if (xTaskCreate(checkpoint_task, "checkpoint", CHECKPOINT_STACK, cp, CHECKPOINT_PRIORITY, NULL) != pdPASS) {
        free(cp);
        printf("CHECKPOINT disabled: no memory for the task\n");
        return;
    }
    s_cp = cp;
    checkpoint_link_start();
#if !CONFIG_C5VRX_VIDEO_OUTPUT
    printf("CHECKPOINT video_output=off (CONFIG_C5VRX_VIDEO_OUTPUT=n): no local output\n");
#endif
}

void checkpoint_print(void)
{
    checkpoint_t *cp = s_cp;
    if (!cp) { printf("CHECKPOINT off\n"); return; }
    const pd_config_t *c = &cp->sd.det.cfg;
    const unsigned top = sd_strongest(&cp->sd);
    printf("CHECKPOINT source=scan live=%u gain=%u top=R%u rise_db=%.1f state=%s sweeps=%lu no_data=%lu passes=%lu "
           "alpha=%.2f active_db=%.1f hyst_db=%.1f drop_db=%.1f min_peak_db=%.1f refractory_ms=%lu stale_ms=%lu\n",
           cp->live, scan_gain(), top + 1u, sd_rise_db(&cp->sd, top), pd_state_name(cp->sd.det.ch[top].state),
           (unsigned long)cp->sweeps, (unsigned long)cp->no_data, (unsigned long)cp->passes, c->alpha, c->active_db,
           c->hysteresis_db, c->drop_db, c->min_peak_db, (unsigned long)c->refractory_ms, (unsigned long)c->stale_ms);
    if (cp->has_last)
        printf("CHECKPOINT last_pass channel=R%u peak_db=%.1f peak_ms=%lu\n", cp->last.channel + 1u,
               cp->last.peak_db, (unsigned long)cp->last.peak_ms);
}

static void prefs_save(const checkpoint_t *cp)
{
    const checkpoint_prefs_t p = {.version = PREFS_VERSION, .det = cp->wanted};
    if (!blob_store("checkpoint", &p, sizeof(p))) printf("CHECKPOINT warning: settings not saved (NVS)\n");
}

/* ---- C5<->C3 link accessors (checkpoint_link.h), called from the link task ---- */

static int32_t to_x10(float v) { return (int32_t)(v * 10.0f + (v >= 0 ? 0.5f : -0.5f)); }

static void config_to_lp(const pd_config_t *c, lp_config_t *o)
{
    o->alpha_milli       = (int32_t)(c->alpha * 1000.0f + 0.5f);
    o->active_db_x10     = to_x10(c->active_db);
    o->hysteresis_db_x10 = to_x10(c->hysteresis_db);
    o->drop_db_x10       = to_x10(c->drop_db);
    o->min_peak_db_x10   = to_x10(c->min_peak_db);
    o->refractory_ms     = c->refractory_ms;
    o->stale_ms          = c->stale_ms;
}

static void lp_to_config(const lp_config_t *o, pd_config_t *c)
{
    c->alpha         = (float)o->alpha_milli / 1000.0f;
    c->active_db     = (float)o->active_db_x10 / 10.0f;
    c->hysteresis_db = (float)o->hysteresis_db_x10 / 10.0f;
    c->drop_db       = (float)o->drop_db_x10 / 10.0f;
    c->min_peak_db   = (float)o->min_peak_db_x10 / 10.0f;
    c->refractory_ms = o->refractory_ms;
    c->stale_ms      = o->stale_ms;
}

/* volume_pct/muted belong to the bridge-side speaker (link_proto keeps them);
 * this detector has none, so it ignores them and reports no sound. */
bool checkpoint_link_apply_config(const lp_config_t *o)
{
    checkpoint_t *cp = s_cp;
    if (!cp) return false;
    pd_config_t c;
    lp_to_config(o, &c);
    if (!pd_config_valid(&c)) return false;
    taskENTER_CRITICAL(&s_wanted_lock);
    cp->wanted = c;
    cp->wanted_dirty = true;
    taskEXIT_CRITICAL(&s_wanted_lock);
    prefs_save(cp);
    return true;
}

void checkpoint_link_get_config(lp_config_t *out)
{
    const checkpoint_t *cp = s_cp;
    pd_config_t c = cp ? cp->wanted : sd_config_default();
    config_to_lp(&c, out);
    out->volume_pct = 0;
    out->muted = 0;
}

/* What T would show, as reads only (no print, no allocation, no receiver write). */
void checkpoint_link_get_stats(lp_stats_t *out)
{
    const checkpoint_t *cp = s_cp;
    const bool live = cp && cp->live;
    const unsigned top = live ? sd_strongest(&cp->sd) : 0u;
    *out = (lp_stats_t){
        .cn_x10   = live ? to_x10(sd_rise_db(&cp->sd, top)) : LP_CN_UNKNOWN,
        .channel  = (uint8_t)top,
        .state    = live ? (uint8_t)cp->sd.det.ch[top].state : 0u,
        .flags    = (uint8_t)((calibration_dc_needed(NULL, 0) ? LP_HEALTH_CAL_NEEDED : 0u) |
                              (calibration_in_startup() ? LP_HEALTH_CALIBRATING : 0u)),
        .gain     = live ? scan_gain() : s_current_gain,
        .passes   = cp ? cp->passes : 0u,
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
        .heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),   /* the boot peak */
        .faults   = transport_fault_count(),
    };
    for (unsigned ch = 0; ch < LP_LEVELS; ch++) {
        const float rise = live ? sd_rise_db(&cp->sd, ch) : 0.0f;
        out->levels[ch] = !live ? LP_LEVEL_UNKNOWN : rise > 127.0f ? 127 : rise < -127.0f ? -127 : (int8_t)lroundf(rise);
    }
    snprintf(out->fw, sizeof(out->fw), "%s", firmware_id());   /* truncated to LP_FW_LEN, NUL-terminated */
}

/* The console's B / F / reset, from the bridge's web (which asked for the VTXs
 * off). filter_request* only raise the calibration task's flags and refuse
 * while one runs ("COMMAND B refused=busy"); the gate is blind until it ends. */
void checkpoint_link_command(uint8_t cmd)
{
    switch (cmd) {
    case LP_CMD_CAL_FULL:
        printf("CHECKPOINT command=cal_full from=link\n");
        filter_request_full();
        break;
    case LP_CMD_CAL_QUICK:
        printf("CHECKPOINT command=cal_quick from=link\n");
        filter_request();
        break;
    case LP_CMD_REBOOT:
        printf("CHECKPOINT command=reboot from=link\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));   /* let the line out over USB */
        esp_restart();
    default:
        printf("CHECKPOINT command=%u refused=unknown from=link\n", cmd);
        break;
    }
}

/* The Waveshare ESP32-C5-Zero's antenna switch: high = IPEX, low = onboard. Left
 * alone unless asked for: other boards use GPIO 26 otherwise. */
#define ANTENNA_GPIO GPIO_NUM_26
static bool s_antenna_ipex;

static void antenna_select(bool ipex)
{
    gpio_reset_pin(ANTENNA_GPIO);
    gpio_set_direction(ANTENNA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ANTENNA_GPIO, ipex);
    s_antenna_ipex = ipex;
    printf("CHECKPOINT antenna=%s (GPIO %d %s)\n", ipex ? "ipex" : "onboard", ANTENNA_GPIO, ipex ? "high" : "low");
}

void checkpoint_antenna_boot(void)
{
#if CONFIG_CHECKPOINT_ANTENNA_IPEX
    antenna_select(true);
#endif
}

void checkpoint_help(void)
{
    printf("CHECKPOINT K + key (lower = down, upper = up; K stays open 5 s after each key, q closes):\n"
           "  a/A smoothing alpha -/+0.05   t/T activity threshold -/+1 dB   h/H hysteresis -/+0.5 dB\n"
           "  d/D pass drop -/+0.5 dB        m/M minimum peak -/+1 dB         r/R refractory -/+250 ms\n"
           "  l live rise of R1-R8 on/off (5 lines/s)   0 defaults   k show   g/G scan gain -/+4 (not saved)\n"
           "  s hop bench: time R1-R8 retunes, no detection for about 15 s (docs/bench.md)\n"
           "  S scan recording: start, then K S again to stop and print; no detection meanwhile (docs/bench.md)\n"
           "  z residue trace: what a strong channel leaves after a hop (docs/bench.md)\n"
           "  i antenna onboard/IPEX, C5-Zero only: drives GPIO 26 (not saved)\n");
    checkpoint_print();
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool checkpoint_key(char key)
{
    checkpoint_t *cp = s_cp;
    if (!cp) return false;   /* checkpoint_help already said "CHECKPOINT off" */
    pd_config_t c = cp->wanted;
    switch (key) {
    case 'a': case 'A': c.alpha = clampf(c.alpha + (key == 'A' ? 0.05f : -0.05f), 0.05f, 1.0f); break;
    case 't': case 'T': c.active_db += key == 'T' ? 1.0f : -1.0f; break;
    case 'h': case 'H': c.hysteresis_db = clampf(c.hysteresis_db + (key == 'H' ? 0.5f : -0.5f), 0.5f, 20.0f); break;
    case 'd': case 'D': c.drop_db = clampf(c.drop_db + (key == 'D' ? 0.5f : -0.5f), 0.5f, 20.0f); break;
    case 'm': case 'M': c.min_peak_db += key == 'M' ? 1.0f : -1.0f; break;
    case 'r': case 'R': {
        const int32_t ms = (int32_t)c.refractory_ms + (key == 'R' ? 250 : -250);
        c.refractory_ms = (uint32_t)(ms < 0 ? 0 : ms > 30000 ? 30000 : ms);
        break;
    }
    case 'l':
        cp->trace = !cp->trace;
        printf("CHECKPOINT trace %s\n", cp->trace ? "ON" : "OFF");
        return true;
    case '0': c = sd_config_default(); break;
    case 'k': case '?': checkpoint_print(); return true;
    case 's': hop_bench_start(); return true;
    case 'S': scan_bench_toggle(); return true;
    case 'g': case 'G': scan_bench_gain(key == 'G'); return true;
    case 'z': residue_bench_start(); return true;
    case 'i': antenna_select(!s_antenna_ipex); return true;
    default: return false;
    }
    if (!pd_config_valid(&c)) {
        printf("CHECKPOINT refused: minimum peak %.1f dB must stay at or above the activity threshold %.1f dB\n",
               c.min_peak_db, c.active_db);
        return true;
    }
    taskENTER_CRITICAL(&s_wanted_lock);
    cp->wanted = c;
    cp->wanted_dirty = true;
    taskEXIT_CRITICAL(&s_wanted_lock);
    prefs_save(cp);
    printf("CHECKPOINT set alpha=%.2f active_db=%.1f hyst_db=%.1f drop_db=%.1f min_peak_db=%.1f refractory_ms=%lu "
           "(saved)\n", c.alpha, c.active_db, c.hysteresis_db, c.drop_db, c.min_peak_db,
           (unsigned long)c.refractory_ms);
    return true;
}
