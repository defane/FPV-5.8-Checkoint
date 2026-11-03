/* Checkpoint pass detection (checkpoint.h).
 *
 * A priority-1 task (under control/gain_obs at 3 and gain_fast at 4) reads
 * the C/N every 50 ms, feeds components/pass_detector with the tuned
 * channel's number in its band (R1..R8 -> 0..7) and, on a pass, prints a
 * PASS line and reports it over the UART link (checkpoint_link.c). No sample
 * while the receiver is not live (splash, calibration, paused) or the C/N is
 * unknown (gain settling, no noise reference, clipping): the detector then
 * ages the channel out.
 *
 * The detector has no local output: no amplifier (the coin sound is played
 * centrally by the C6 master) and no status LED (the C5's onboard WS2812 sits
 * on a receiver IQ-loopback pin). Console (console.c): K then one key per
 * change tunes the detector (checkpoint_help). Settings are kept in NVS
 * "checkpoint".
 *
 * In production nobody opens this C5's USB console, so the bridge (a C3 node,
 * or the C6 master for its own gate) gets the console's T as health in STATS
 * and its B, F and a reset as link COMMANDs. Apart from those and SET_CHANNEL
 * (a retune, saved in the engine's settings like a console retune) the
 * checkpoint only reads the receiver. */
#include "checkpoint.h"
#include "receiver.h"
#include <stdio.h>
#include <stdlib.h>
#include "checkpoint_link.h"
#include "cn_estimate.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "pass_detector.h"
#include "rf.h"
#include "sdkconfig.h"

#define CHECKPOINT_TICK_MS   50u
#define CHECKPOINT_PRIORITY  1u     /* below every receiver task */
#define CHECKPOINT_STACK     3072u

/* NVS "checkpoint"; a new layout gets a new version (old blobs ignored). The
 * three bytes after the version held the volume and mute of the removed local
 * sound: kept as padding, so saved detector settings still load. */
#define PREFS_VERSION 1u
typedef struct {
    uint8_t version, reserved[3];
    pd_config_t det;
} checkpoint_prefs_t;

typedef struct {
    pass_detector_t det;
    pd_config_t wanted;              /* console copy; the task applies it */
    volatile bool wanted_dirty, trace;
    uint32_t valid, unknown, passes;
    bool has_last;
    pd_event_t last;
    const char *last_name;           /* its channel, e.g. "R3" */
} checkpoint_t;

static checkpoint_t *s_cp;
static portMUX_TYPE s_wanted_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void on_pass(checkpoint_t *cp, const pd_event_t *e)
{
    const fpv_channel_t *ch = rf_channel();
    ++cp->passes;
    cp->last = *e;
    cp->last_name = ch->name;
    cp->has_last = true;
    printf("PASS channel=%s freq=%u peak_cn_db=%.1f peak_ms=%lu at_ms=%lu delay_ms=%lu\n",
           ch->name, ch->freq_mhz, e->peak_db, (unsigned long)e->peak_ms, (unsigned long)e->at_ms,
           (unsigned long)(e->at_ms - e->peak_ms));
    checkpoint_link_report_pass(e);
}

/* A live receiver sample, in dB; false when there is none. */
static bool sample_cn(float *db)
{
    if (s_menu_active || s_scan_active || menu_status_active() || receiver_paused() || !receiver_active()) return false;
    const int x10 = receiver_obs_cn_x10();
    if (x10 == CN_UNKNOWN_X10) return false;
    *db = (float)x10 / 10.0f;
    return true;
}

static void checkpoint_task(void *arg)
{
    checkpoint_t *cp = arg;
    uint32_t seen_epoch = s_control_epoch;
    size_t seen_index = rf_channel_index();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECKPOINT_TICK_MS));
        const uint32_t now = now_ms();
        const size_t index = rf_channel_index();
        /* Retune, scan or calibration: whatever was in progress is not a pass. */
        if (s_control_epoch != seen_epoch || index != seen_index) {
            seen_epoch = s_control_epoch;
            seen_index = index;
            pd_reset_all(&cp->det);
        }
        if (cp->wanted_dirty) {
            pd_config_t c;
            taskENTER_CRITICAL(&s_wanted_lock);
            c = cp->wanted;
            cp->wanted_dirty = false;
            taskEXIT_CRITICAL(&s_wanted_lock);
            (void)pd_set_config(&cp->det, &c);   /* validated by the console */
        }
        const unsigned slot = (unsigned)(index % 8u);
        float db;
        if (!sample_cn(&db)) {
            ++cp->unknown;
            pd_expire(&cp->det, now);
            if (cp->trace) printf("CP_TRACE t_ms=%lu ch=%s cn_db=unknown state=%s\n", (unsigned long)now,
                                  rf_channel()->name, pd_state_name(cp->det.ch[slot].state));
            continue;
        }
        ++cp->valid;
        pd_event_t e;
        const bool pass = pd_update(&cp->det, slot, db, now, &e);
        if (cp->trace) printf("CP_TRACE t_ms=%lu ch=%s cn_db=%.1f smooth_db=%.1f peak_db=%.1f state=%s\n",
                              (unsigned long)now, rf_channel()->name, db, cp->det.ch[slot].smooth,
                              cp->det.ch[slot].peak, pd_state_name(cp->det.ch[slot].state));
        if (pass) on_pass(cp, &e);
    }
}

void checkpoint_start(void)
{
    if (s_cp) return;
    checkpoint_t *cp = calloc(1, sizeof(*cp));
    if (!cp) { printf("CHECKPOINT disabled: no memory\n"); return; }
    checkpoint_prefs_t prefs;
    const bool saved = blob_load("checkpoint", &prefs, sizeof(prefs)) && prefs.version == PREFS_VERSION &&
                       pd_config_valid(&prefs.det);
    const pd_config_t cfg = saved ? prefs.det : pd_config_default();
    pd_init(&cp->det, &cfg);
    cp->wanted = cp->det.cfg;
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
    const pd_config_t *c = &cp->det.cfg;
    const unsigned slot = (unsigned)(rf_channel_index() % 8u);
    printf("CHECKPOINT source=cn state=%s smooth_db=%.1f peak_db=%.1f samples_valid=%lu unknown=%lu passes=%lu "
           "alpha=%.2f active_db=%.1f hyst_db=%.1f drop_db=%.1f min_peak_db=%.1f refractory_ms=%lu stale_ms=%lu\n",
           pd_state_name(cp->det.ch[slot].state), cp->det.ch[slot].smooth, cp->det.ch[slot].peak,
           (unsigned long)cp->valid, (unsigned long)cp->unknown, (unsigned long)cp->passes, c->alpha, c->active_db,
           c->hysteresis_db, c->drop_db, c->min_peak_db, (unsigned long)c->refractory_ms, (unsigned long)c->stale_ms);
    if (cp->has_last)
        printf("CHECKPOINT last_pass channel=%s peak_cn_db=%.1f at_ms=%lu\n", cp->last_name,
               cp->last.peak_db, (unsigned long)cp->last.at_ms);
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
    pd_config_t c = cp ? cp->wanted : pd_config_default();
    config_to_lp(&c, out);
    out->volume_pct = 0;
    out->muted = 0;
}

/* What T would show, as reads only (no print, no allocation, no receiver write). */
void checkpoint_link_get_stats(lp_stats_t *out)
{
    const checkpoint_t *cp = s_cp;
    const int x10 = receiver_obs_cn_x10();
    const unsigned slot = (unsigned)(rf_channel_index() % 8u);
    *out = (lp_stats_t){
        .cn_x10   = (x10 == CN_UNKNOWN_X10) ? LP_CN_UNKNOWN : x10,
        .channel  = (uint8_t)slot,
        .state    = cp ? (uint8_t)cp->det.ch[slot].state : 0u,
        .flags    = (uint8_t)((calibration_dc_needed(NULL, 0) ? LP_HEALTH_CAL_NEEDED : 0u) |
                              (calibration_in_startup() ? LP_HEALTH_CALIBRATING : 0u)),
        .gain     = s_current_gain,
        .passes   = cp ? cp->passes : 0u,
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
        .heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),   /* the boot peak */
        .faults   = transport_fault_count(),
    };
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

void checkpoint_help(void)
{
    printf("CHECKPOINT K + key (lower = down, upper = up; K stays open 5 s after each key, q closes):\n"
           "  a/A smoothing alpha -/+0.05   t/T activity threshold -/+1 dB   h/H hysteresis -/+0.5 dB\n"
           "  d/D pass drop -/+0.5 dB        m/M minimum peak -/+1 dB         r/R refractory -/+250 ms\n"
           "  l live C/N trace on/off (20 lines/s)   0 defaults   k show\n");
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
    case '0': c = pd_config_default(); break;
    case 'k': case '?': checkpoint_print(); return true;
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
