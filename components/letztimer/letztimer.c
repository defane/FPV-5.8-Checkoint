/* LetzTimer pass detection (letztimer.h): state, saved settings, the console's K keys, the
 * link accessors and the commands.
 *
 * The scan task (lt_scan.c) owns the sdr_rx engine and the detector: it reads R1-R8 in LO pairs,
 * ~500 times a second per channel, and nobody picks a channel: every drone on Raceband is seen.
 * The console (K keys, F, B) and the link (CONFIG_SET, COMMAND) only post requests here; the scan
 * task applies them between two hops. A calibration or a console bench runs inside the scan
 * task: no detection meanwhile, and the rests are learnt again afterwards.
 *
 * The detector has no local output: no amplifier (the coin sound is played centrally by the C6
 * master) and no status LED. Settings are kept in NVS namespace "letztimer", key "prefs".
 *
 * In production nobody opens this C5's USB console, so the bridge (a C3 node, or the C6 master
 * for its own gate) gets the console's T as health in STATS and its B, F and a reset as link
 * COMMANDs. */
#include "letztimer.h"
#include "lt_log.h"
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "letztimer_link.h"
#include "lt_bench.h"
#include "lt_internal.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

/* NVS "letztimer" key "prefs"; a new layout or meaning gets a new version (old blobs ignored,
 * but for the one migration below).
 * 2: dB of rise above a channel's rest, no longer of C/N (C5VRX, namespace c5vrx5).
 * 3: the sdr_rx meter (+-5 MHz band power, 10-bit, clipping instead of folding): a threshold
 *    tuned on the old meter is not trusted; the gain takes the first reserved byte.
 * 4: the relative re-arm's drop, in the two bytes version 3 kept reserved (zero there). Same size
 *    and offsets, so a version-3 record is read as it is, the drop taken from the build. */
#define PREFS_VERSION 4u
#define PREFS_V3 3u
#define PREFS_KEY "prefs"
typedef struct {
    uint8_t version, gain;
    uint16_t rearm_db_x10;   /* 4: the relative re-arm's drop * 10, 0..LP_REARM_MAX_X10 */
    pd_config_t det;
} letztimer_prefs_t;
_Static_assert(offsetof(letztimer_prefs_t, rearm_db_x10) == 2 && offsetof(letztimer_prefs_t, det) == 4,
               "version 3's layout: version, gain, 2 reserved bytes, then det (the migration reads it in place)");
_Static_assert(CONFIG_LETZTIMER_REARM_DROP_DB >= 0 && CONFIG_LETZTIMER_REARM_DROP_DB * 10 <= LP_REARM_MAX_X10,
               "the build's re-arm drop fits the link's range");
_Static_assert(PD_IDLE == LP_STATE_IDLE && PD_RISING == LP_STATE_RISING && PD_PEAK_PASSED == LP_STATE_PEAK_PASSED &&
               PD_COOLDOWN == LP_STATE_COOLDOWN && SDR_RX_CHANNELS <= LP_LEVELS,
               "STATS carries each channel's pd_state_t as it is, in 2 bits");
#define LT_REARM_DEFAULT_DB ((float)CONFIG_LETZTIMER_REARM_DROP_DB)   /* first boot, a v3 record, K 0 */

portMUX_TYPE lt_mux = portMUX_INITIALIZER_UNLOCKED;
static lt_t *s_cp;
static volatile const char *s_stage = "start";
static uint32_t s_stalls;            /* stall onsets: the link task's (letztimer_link_get_stats) */
static bool s_was_stalled;

lt_t *lt_state(void) { return s_cp; }

void letztimer_boot_stage(const char *stage) { s_stage = stage; }
const char *lt_boot_stage(void) { return (const char *)s_stage; }

void letztimer_reserve(void)
{
    if (!lt_scan_reserve()) lt_printf("LETZTIMER warning: no memory for the scan task's stack\n");
}

void lt_health(const lt_t *cp, bool *live, bool *stalled)
{
    /* last_rotation_ms first: read after it, now is never earlier, so the difference cannot wrap
     * when the scan task writes it in between. */
    const uint32_t last = cp->last_rotation_ms;
    const bool calibrating = sdr_rx_calibrating();
    const bool fresh = lt_now_ms() - last < LT_STALL_MS;
    *live = cp->live && !cp->bench_running && !cp->engine_failed && !calibrating && fresh;
    *stalled = cp->live && !cp->bench_running && !calibrating && !fresh;
}

void lt_mark_read(lt_t *cp, uint32_t now_ms)
{
    for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) cp->last_reading_ms[ch] = now_ms;
    cp->last_rotation_ms = now_ms;
}

static int32_t to_x10(float v) { return (int32_t)(v * 10.0f + (v >= 0 ? 0.5f : -0.5f)); }

/* A version-3 record keeps every value the operator tuned, with the build's drop: the next save
 * writes version 4. */
static void prefs_load(pd_config_t *det, uint8_t *gain, float *rearm_db)
{
    letztimer_prefs_t p;
    const bool read = lt_blob_load(PREFS_KEY, &p, sizeof(p));
    const bool v4 = read && p.version == PREFS_VERSION, saved = v4 || (read && p.version == PREFS_V3);
    *gain = saved && p.gain >= LT_GAIN_MIN && p.gain <= 83u ? p.gain : (uint8_t)CONFIG_SDR_RX_GAIN_DEFAULT;
    *det = saved && pd_config_valid(&p.det) ? p.det : sd_config_default();
    *rearm_db = v4 && p.rearm_db_x10 <= LP_REARM_MAX_X10 ? (float)p.rearm_db_x10 / 10.0f : LT_REARM_DEFAULT_DB;
    lt_printf("LETZTIMER prefs=%s gain=%u rearm_db=%.1f\n", v4 ? "saved" : saved ? "saved_v3" : "defaults", *gain,
           (double)*rearm_db);
}

static void prefs_save(const lt_t *cp)
{
    letztimer_prefs_t p = {.version = PREFS_VERSION};
    portENTER_CRITICAL(&lt_mux);
    p.gain = cp->gain;
    p.det = cp->wanted;
    const float rearm = cp->wanted_rearm_db;
    portEXIT_CRITICAL(&lt_mux);
    p.rearm_db_x10 = (uint16_t)to_x10(rearm);   /* 0..LP_REARM_MAX_X10: checked by every poster */
    if (!lt_blob_store(PREFS_KEY, &p, sizeof(p))) lt_printf("LETZTIMER warning: settings not saved (NVS)\n");
}

void letztimer_start(int64_t boot_t0_us)
{
    if (s_cp) return;
    letztimer_boot_stage("letztimer");
    lt_t *cp = calloc(1, sizeof(*cp));
    if (!cp) { lt_printf("LETZTIMER disabled: no memory\n"); return; }
    cp->recal_req = -1;
    prefs_load(&cp->wanted, &cp->gain, &cp->wanted_rearm_db);
    sd_init(&cp->sd, &cp->wanted);
    pd_set_rearm(&cp->sd.det, cp->wanted_rearm_db);   /* the scan task starts it again from the mailbox */
    if (!lt_scan_start(cp, boot_t0_us)) {
        free(cp);
        lt_printf("LETZTIMER disabled: no memory for the scan task\n");
        return;
    }
    s_cp = cp;
    letztimer_link_start();
}

void letztimer_print(void)
{
    lt_t *cp = s_cp;
    if (!cp) { lt_printf("LETZTIMER off\n"); return; }
    bool live, stalled;
    lt_health(cp, &live, &stalled);
    const pd_config_t *c = &cp->sd.det.cfg;
    const unsigned top = sd_strongest(&cp->sd);
    lt_printf("LETZTIMER source=sdr live=%u learning=%u bench=%u engine_failed=%u stalled=%u stalls=%" PRIu32
           " gain=%u top=R%u rise_db=%.1f state=%s rotations=%" PRIu32 " readings=%" PRIu32 " passes=%" PRIu32
           " alpha=%.2f tau_ms=%.1f active_db=%.1f hyst_db=%.1f drop_db=%.1f min_peak_db=%.1f refractory_ms=%" PRIu32
           " stale_ms=%" PRIu32 " rearm_db=%.1f\n",
           live, cp->learning, cp->bench_running, cp->engine_failed, stalled, s_stalls, (unsigned)cp->gain, top + 1u,
           (double)sd_rise_db(&cp->sd, top), pd_state_name(cp->sd.det.ch[top].state), cp->rotations, cp->readings,
           cp->passes, (double)c->alpha, (double)sd_tau_ms(&cp->sd), (double)c->active_db, (double)c->hysteresis_db,
           (double)c->drop_db, (double)c->min_peak_db, c->refractory_ms, c->stale_ms, (double)cp->sd.det.rearm_db);
    if (cp->has_last)
        lt_printf("LETZTIMER last_pass channel=R%u peak_db=%.1f peak_ms=%" PRIu32 "\n", cp->last.channel + 1u,
               (double)cp->last.peak_db, cp->last.peak_ms);
}

/* ---- requests to the scan task (lt_internal.h mailbox) ---- */

/* c valid (pd_config_valid) and rearm_db in [0, LP_REARM_MAX_X10 / 10]: the posters check. */
static void post_config(lt_t *cp, const pd_config_t *c, float rearm_db)
{
    portENTER_CRITICAL(&lt_mux);
    cp->wanted = *c;
    cp->wanted_rearm_db = rearm_db;
    cp->wanted_dirty = true;                  /* accepted during a bench too: applied after it */
    portEXIT_CRITICAL(&lt_mux);
    prefs_save(cp);
}

/* A bench owns the engine from the moment it is posted. */
static bool bench_owns(const lt_t *cp) { return cp->bench_running || cp->bench_req; }

static bool post_recal(lt_t *cp, sdr_recal_t kind, const char *from)
{
    const char *name = kind == SDR_RECAL_FULL ? "cal_full" : "cal_quick";
    if (!cp || cp->engine_failed) {
        lt_printf("LETZTIMER command=%s refused=no_engine from=%s\n", name, from);
        return false;
    }
    const bool calibrating = sdr_rx_calibrating();
    portENTER_CRITICAL(&lt_mux);
    const bool busy = calibrating || bench_owns(cp) || cp->recal_req >= 0;
    if (!busy) {
        cp->recal_req = (int8_t)kind;
        cp->recal_from = from;
    }
    portEXIT_CRITICAL(&lt_mux);
    lt_printf(busy ? "LETZTIMER command=%s refused=busy from=%s\n" : "LETZTIMER command=%s from=%s\n", name, from);
    return !busy;
}

void lt_command(uint8_t cmd, const char *from)
{
    switch (cmd) {
    case LP_CMD_CAL_FULL:  (void)post_recal(s_cp, SDR_RECAL_FULL, from); break;
    case LP_CMD_CAL_QUICK: (void)post_recal(s_cp, SDR_RECAL_QUICK, from); break;
    case LP_CMD_REBOOT:
        lt_printf("LETZTIMER command=reboot from=%s\n", from);
        sdr_rx_reboot();                      /* quiesces the captures, never returns */
    default:
        lt_printf("LETZTIMER command=%u refused=unknown from=%s\n", cmd, from);
        break;
    }
}

static void post_bench(lt_t *cp, char key)
{
    if (key == 'S' && cp->bench_running && cp->bench_key == 'S') {   /* the recording runs: stop it */
        cp->scan_stop = true;
        lt_printf("LETZTIMER bench=S stop\n");
        return;
    }
    const bool calibrating = sdr_rx_calibrating();
    const bool ready = cp->live && !cp->engine_failed;
    portENTER_CRITICAL(&lt_mux);
    const bool busy = !ready || calibrating || bench_owns(cp) || cp->recal_req >= 0 || cp->gain_dirty;
    if (!busy) cp->bench_req = (uint8_t)key;
    portEXIT_CRITICAL(&lt_mux);
    if (busy) lt_printf("LETZTIMER bench=%c refused=%s\n", key, ready ? "busy" : "not_ready");
    else lt_printf("LETZTIMER bench=%c start\n", key);
}

/* The console's K g/G (a step from the saved gain) and the link's GAIN (an index), the same way. */
static void post_gain_to(lt_t *cp, int want, bool step, bool up)
{
    if (bench_owns(cp)) { lt_printf("LETZTIMER gain refused=busy\n"); return; }
    const unsigned max = sdr_rx_gain_max();
    portENTER_CRITICAL(&lt_mux);
    int g = step ? (int)cp->gain + (up ? (int)LT_GAIN_STEP : -(int)LT_GAIN_STEP) : want;
    if (g > (int)max) g = (int)max;
    if (g < (int)LT_GAIN_MIN) g = (int)LT_GAIN_MIN;
    cp->gain = (uint8_t)g;
    cp->gain_dirty = true;
    portEXIT_CRITICAL(&lt_mux);
    prefs_save(cp);
    lt_printf("LETZTIMER gain=%d/%u (saved; the rests are learnt again)\n", g, max);
}

static void post_gain(lt_t *cp, bool up) { post_gain_to(cp, 0, true, up); }

/* ---- C5<->C3 link accessors (letztimer_link.h), called from the link task ---- */

static void config_to_lp(const pd_config_t *c, float rearm_db, lp_config_t *o)
{
    o->alpha_milli       = (int32_t)(c->alpha * 1000.0f + 0.5f);
    o->active_db_x10     = to_x10(c->active_db);
    o->hysteresis_db_x10 = to_x10(c->hysteresis_db);
    o->drop_db_x10       = to_x10(c->drop_db);
    o->min_peak_db_x10   = to_x10(c->min_peak_db);
    o->refractory_ms     = c->refractory_ms;
    o->stale_ms          = c->stale_ms;
    o->rearm_db_x10      = to_x10(rearm_db);
}

/* alpha_milli: the weight per 17.5 ms reference sweep (time constant -17.5 ms / ln(1 - alpha)),
 * which is what it was at C5VRX's 57 Hz sweep. */
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
 * this detector has none, so it ignores them and reports no sound. A value out of range
 * refuses the whole CONFIG_SET, the re-arm drop's (0..LP_REARM_MAX_X10) as the others: the
 * echo then shows the settings kept. */
bool letztimer_link_apply_config(const lp_config_t *o)
{
    lt_t *cp = s_cp;
    if (!cp) return false;
    pd_config_t c;
    lp_to_config(o, &c);
    if (!pd_config_valid(&c) || o->rearm_db_x10 < 0 || o->rearm_db_x10 > LP_REARM_MAX_X10) return false;
    post_config(cp, &c, (float)o->rearm_db_x10 / 10.0f);
    return true;
}

void letztimer_link_get_config(lp_config_t *out)
{
    const lt_t *cp = s_cp;
    pd_config_t c = sd_config_default();
    float rearm = LT_REARM_DEFAULT_DB;
    if (cp) {
        portENTER_CRITICAL(&lt_mux);
        c = cp->wanted;
        rearm = cp->wanted_rearm_db;
        portEXIT_CRITICAL(&lt_mux);
    }
    config_to_lp(&c, rearm, out);
    out->volume_pct = 0;
    out->muted = 0;
}

/* What T would show, as reads only (no print, no allocation, no engine call but its [any] reads).
 * live is computed now rather than trusted from the scan task: a scan task that stopped rotating
 * outside a calibration or a bench is "stalled", which flags CAL_NEEDED and counts a fault. A
 * channel with no reading for LT_STALL_MS while the gate rotates reads LP_LEVEL_UNKNOWN, not a
 * rise of 0: its LO's captures fail (the engine flags CAL_NEEDED after 3 s and the faults count
 * them), or the pair guard held every reading of it (a held reading counts as none, lt_scan.c:
 * its partner on the same LO clips the ADC, a drone hovering by the gate; no fault, no flag). */
void letztimer_link_get_stats(lp_stats_t *out)
{
    const lt_t *cp = s_cp;
    bool live = false, stalled = false;
    if (cp) lt_health(cp, &live, &stalled);
    if (stalled && !s_was_stalled) s_stalls++;
    s_was_stalled = stalled;
    sdr_rx_health_t h;
    sdr_rx_get_health(&h);
    const bool engine_failed = cp && cp->engine_failed;
    const bool blind = sdr_rx_calibrating() || (cp && (cp->learning || cp->bench_running));
    const unsigned top = live ? sd_strongest(&cp->sd) : 0u;
    uint16_t states = 0;   /* all IDLE unless the gate scans */
    for (unsigned ch = 0; live && ch < SDR_RX_CHANNELS; ch++)
        states = lp_state_set(states, ch, (unsigned)cp->sd.det.ch[ch].state);
    *out = (lp_stats_t){
        .cn_x10   = live ? to_x10(sd_rise_db(&cp->sd, top)) : LP_CN_UNKNOWN,
        .states   = states,
        .flags    = (uint8_t)((h.cal_needed || engine_failed || stalled ? LP_HEALTH_CAL_NEEDED : 0u) |
                              (blind ? LP_HEALTH_CALIBRATING : 0u)),
        .gain     = sdr_rx_gain(),
        .passes   = cp ? cp->passes : 0u,
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
        .heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),   /* the boot peak */
        .faults   = sdr_rx_fault_count() + s_stalls,
    };
    for (unsigned ch = 0; ch < LP_LEVELS; ch++) {
        bool heard = live && ch < SDR_RX_CHANNELS;
        if (heard) {
            const uint32_t last = cp->last_reading_ms[ch];   /* first: now is never earlier (no wrap) */
            heard = lt_now_ms() - last < LT_STALL_MS;
        }
        const float rise = heard ? sd_rise_db(&cp->sd, ch) : 0.0f;
        out->levels[ch] = !heard ? LP_LEVEL_UNKNOWN : rise > 127.0f ? 127 : rise < -127.0f ? -127 : (int8_t)lroundf(rise);
    }
    snprintf(out->fw, sizeof(out->fw), "%s", firmware_id());   /* truncated to LP_FW_LEN, NUL-terminated */
}

/* The console's B / F / reset, from the bridge's web (which asked for the VTXs off). A
 * recalibration only posts a request (refused while one is pending or running, or a bench
 * runs: "command=cal_full refused=busy"); the gate is blind until it and the relearn end. */
void letztimer_link_command(uint8_t cmd) { lt_command(cmd, "link"); }

void letztimer_link_calib_start(uint8_t step, uint8_t channel)
{
    if (s_cp) lt_calib_request(s_cp, step, channel);
}

bool letztimer_link_get_calib(lp_calib_t *out)
{
    return s_cp && lt_calib_get(s_cp, out);
}

_Static_assert(LT_GAIN_MIN == LP_GAIN_MIN, "the link and the console clamp at the same lowest gain");

/* GAIN from the bridge's web: the K g/G path, to that index (clamped to LT_GAIN_MIN..the PHY's
 * maximum, saved, the rests learnt again: the gate is blind a moment; refused while a bench runs). */
void letztimer_link_set_gain(uint8_t gain)
{
    if (s_cp) post_gain_to(s_cp, gain, false, false);
}

/* The Waveshare ESP32-C5-Zero's antenna switch: high = IPEX, low = onboard. Left
 * alone unless asked for: other boards use GPIO 26 otherwise. */
#define ANTENNA_GPIO GPIO_NUM_26
static bool s_antenna_ipex;

static void antenna_select(bool ipex, const char *then)
{
    gpio_reset_pin(ANTENNA_GPIO);
    gpio_set_direction(ANTENNA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ANTENNA_GPIO, ipex);
    s_antenna_ipex = ipex;
    lt_printf("LETZTIMER antenna=%s (GPIO %d %s)%s\n", ipex ? "ipex" : "onboard", ANTENNA_GPIO, ipex ? "high" : "low",
           then);
}

/* Before the radio starts: the PHY's boot calibration and the LO calibrations see that antenna. */
void letztimer_antenna_boot(void)
{
#if CONFIG_LETZTIMER_ANTENNA_IPEX
    antenna_select(true, "");
#endif
}

/* K i: the other antenna, then every LO recalibrated on it and the rests learnt again. */
static void antenna_toggle(lt_t *cp)
{
    const bool calibrating = sdr_rx_calibrating();
    if (cp->engine_failed || calibrating || bench_owns(cp) || cp->recal_req >= 0) {
        lt_printf("LETZTIMER antenna refused=busy\n");
        return;
    }
    antenna_select(!s_antenna_ipex, " recal=quick");
    (void)post_recal(cp, SDR_RECAL_QUICK, "antenna");
}

void letztimer_help(void)
{
    lt_printf("LETZTIMER K + key (lower = down, upper = up; K stays open 5 s after each key, q closes):\n"
           "  a/A smoothing alpha -/+0.05 (per 17.5 ms)   t/T activity threshold -/+1 dB   h/H hysteresis -/+0.5 dB\n"
           "  d/D pass drop -/+0.5 dB        m/M minimum peak -/+1 dB         r/R refractory -/+250 ms\n"
           "  e/E re-arm drop -/+1 dB (0 = off)   l live rise of R1-R8 on/off (5 lines/s)   0 defaults   k show\n"
           "  g/G RX gain -/+2 (saved; the rests are learnt again)\n"
           "  i antenna onboard/IPEX, C5-Zero only: drives GPIO 26 (not saved), then recalibrates\n"
           "  engine benches, no detection meanwhile (docs/bench.md):\n"
           "  s timing   z settle/residue   x image rejection   n noise   p SINGLE plan   S scan recording start/stop\n");
    letztimer_print();
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool letztimer_key(char key)
{
    lt_t *cp = s_cp;
    if (!cp) return false;   /* letztimer_help already said "LETZTIMER off" */
    if (lt_bench_key(key)) { post_bench(cp, key); return true; }
    pd_config_t c;
    portENTER_CRITICAL(&lt_mux);
    c = cp->wanted;
    float rearm = cp->wanted_rearm_db;
    portEXIT_CRITICAL(&lt_mux);
    switch (key) {
    case 'a': case 'A': c.alpha = clampf(c.alpha + (key == 'A' ? 0.05f : -0.05f), 0.05f, 1.0f); break;
    case 't': case 'T': c.active_db += key == 'T' ? 1.0f : -1.0f; break;
    case 'h': case 'H': c.hysteresis_db = clampf(c.hysteresis_db + (key == 'H' ? 0.5f : -0.5f), 0.5f, 20.0f); break;
    case 'd': case 'D': c.drop_db = clampf(c.drop_db + (key == 'D' ? 0.5f : -0.5f), 0.5f, 20.0f); break;
    case 'm': case 'M': c.min_peak_db += key == 'M' ? 1.0f : -1.0f; break;
    case 'e': case 'E': rearm = clampf(rearm + (key == 'E' ? 1.0f : -1.0f), 0.0f, (float)LP_REARM_MAX_X10 / 10.0f); break;
    case 'r': case 'R': {
        const int32_t ms = (int32_t)c.refractory_ms + (key == 'R' ? 250 : -250);
        c.refractory_ms = (uint32_t)(ms < 0 ? 0 : ms > 30000 ? 30000 : ms);
        break;
    }
    case 'l':
        cp->trace = !cp->trace;
        lt_printf("LETZTIMER trace %s\n", cp->trace ? "ON" : "OFF");
        return true;
    case '0': c = sd_config_default(); rearm = LT_REARM_DEFAULT_DB; break;
    case 'k': case '?': letztimer_print(); return true;
    case 'g': case 'G': post_gain(cp, key == 'G'); return true;
    case 'i': antenna_toggle(cp); return true;
    default: return false;
    }
    if (!pd_config_valid(&c)) {
        lt_printf("LETZTIMER refused: minimum peak %.1f dB must stay at or above the activity threshold %.1f dB\n",
               (double)c.min_peak_db, (double)c.active_db);
        return true;
    }
    post_config(cp, &c, rearm);
    lt_printf("LETZTIMER set alpha=%.2f active_db=%.1f hyst_db=%.1f drop_db=%.1f min_peak_db=%.1f refractory_ms=%" PRIu32
           " rearm_db=%.1f (saved)\n", (double)c.alpha, (double)c.active_db, (double)c.hysteresis_db, (double)c.drop_db,
           (double)c.min_peak_db, c.refractory_ms, (double)rearm);
    return true;
}
