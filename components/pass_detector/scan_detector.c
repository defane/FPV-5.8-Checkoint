/* Scan detector. See scan_detector.h. */
#include "scan_detector.h"
#include <math.h>
#include <string.h>

#define QDB_PER_DB 4.0f   /* level codes per dB (= SDR_QDB_PER_DB) */

_Static_assert(PD_CHANNELS <= 8u, "the pair guard's channel masks are 8 bits");

pd_config_t sd_config_default(void)
{
    return (pd_config_t){.alpha = 0.25f, .active_db = 8.0f, .hysteresis_db = 3.0f, .drop_db = 3.0f,
                         .min_peak_db = 10.0f, .refractory_ms = 1500u, .stale_ms = 1000u};
}

/* alpha is the weight per reference sweep: 1 - exp(-SD_REF_SWEEP_US / tau) = alpha. */
static float tau_of(float alpha)
{
    return alpha >= 1.0f ? 0.0f : -(float)SD_REF_SWEEP_US / logf(1.0f - alpha);
}

void sd_init(scan_detector_t *sd, const pd_config_t *cfg)
{
    memset(sd, 0, sizeof(*sd));
    pd_init(&sd->det, cfg);
    sd->tau_us = tau_of(sd->det.cfg.alpha);
    sd->cap_db = INFINITY;
}

bool sd_set_config(scan_detector_t *sd, const pd_config_t *cfg)
{
    if (!pd_set_config(&sd->det, cfg)) return false;
    sd->tau_us = tau_of(sd->det.cfg.alpha);
    return true;
}

void sd_relearn(scan_detector_t *sd)
{
    memset(sd->c, 0, sizeof(sd->c));
    pd_reset_all(&sd->det);
    sd->cap_db = INFINITY;
    sd->learn_end_us = 0;
    memset(sd->sat_run_us, 0, sizeof(sd->sat_run_us));
    memset(sd->sat_last_us, 0, sizeof(sd->sat_last_us));
    memset(sd->sat_src, 0, sizeof(sd->sat_src));
    memset(sd->raw_n, 0, sizeof(sd->raw_n));
    memset(sd->raw_i, 0, sizeof(sd->raw_i));
    sd->held_mask = 0;
    sd->fresh_mask = 0;
    sd->long_mask = 0;
    sd->settle_mask = 0;
}

static int16_t median3(int16_t a, int16_t b, int16_t c)
{
    const int16_t lo = a < b ? a : b, hi = a < b ? b : a;
    return c < lo ? lo : c > hi ? hi : c;
}

/* The average's weight for a reading dt_us after the previous one: 1 - exp(-x),
 * x = dt / tau, as its (2,2) Pade approximant x / (1 + x/2 + x^2/12) (widscan58's lap
 * EMA, in float): within 1 % up to x = 2, and 1 from x = 4. */
static float weight(float tau_us, int64_t dt_us)
{
    if (tau_us <= 0.0f) return 1.0f;
    const float x = (float)dt_us / tau_us;
    return x >= 4.0f ? 1.0f : x / (1.0f + x * (0.5f + x / 12.0f));
}

/* The rest follows the low smoothed values; never from a saturated capture. It climbs only while
 * the channel is quiet: IDLE, and not armed by the relative re-arm (a drone still well up). */
static void learn_rest(scan_detector_t *sd, unsigned ch, int64_t dt_us, bool learning)
{
    sd_channel_t *c = &sd->c[ch];
    if (!c->rest_primed) {
        c->rest_db = c->smooth_db;
        c->rest_primed = true;
        return;
    }
    const int64_t dt = dt_us < SD_DT_MAX_US ? dt_us : SD_DT_MAX_US;
    const float s = (float)dt / (float)SD_REF_SWEEP_US * (learning ? SD_LEARN_RATE : 1.0f);
    if (c->smooth_db < c->rest_db) c->rest_db -= SD_REST_DOWN_DB * s;
    else if (sd->det.ch[ch].state == PD_IDLE && !sd->det.ch[ch].rel) c->rest_db += SD_REST_UP_DB * s;
    if (c->rest_db > sd->cap_db) c->rest_db = sd->cap_db;
}

bool sd_feed(scan_detector_t *sd, unsigned channel, int16_t level_qdb, uint8_t flags, int64_t t_us,
             pd_event_t *event)
{
    if (channel >= PD_CHANNELS || level_qdb == SD_LEVEL_NONE) return false;
    sd_channel_t *c = &sd->c[channel];
    int64_t dt = 0;
    if (c->primed) {
        dt = t_us - c->last_us;
        if (dt < 0) dt = 0;
    }
    c->last_us = t_us;
    if (c->primed && dt > SD_DT_MAX_US) {
        /* A gap (a calibration): the old readings would come back, so the median starts again.
         * Up to pd's stale_ms the average then resumes where it was, which keeps it and pd's
         * peak one series (a level re-primed from one reading, against a peak built from the
         * smoothed one, rang on a mere noise excursion); after a longer gap pd has forgotten
         * the channel and the average re-primes. */
        c->nhist = 0;
        c->hidx = 0;
        if (dt > (int64_t)sd->det.cfg.stale_ms * 1000) c->primed = false;
    }

    c->hist[c->hidx] = level_qdb;
    c->hidx = c->hidx >= 2u ? 0u : (uint8_t)(c->hidx + 1u);
    if (c->nhist < 3u) c->nhist++;
    /* After a gap, held until the median is full again: a lone burst must not lead the channel. */
    if (c->nhist < 3u && c->rest_primed) return false;
    const int16_t m = c->nhist == 3u ? median3(c->hist[0], c->hist[1], c->hist[2]) : level_qdb;
    const float db = (float)m / QDB_PER_DB;
    if (!c->primed) {                        /* (re-)primed from this median; dt_avg kept */
        c->smooth_db = db;
        c->primed = true;
    } else {
        /* A late reading (dt over twice the usual interval) weighs as one on time: its median
         * mostly holds readings from before the delay. */
        const int64_t most = c->dt_avg_us ? 2 * (int64_t)c->dt_avg_us + 1000 : dt;
        c->smooth_db += weight(sd->tau_us, dt < most ? dt : most) * (db - c->smooth_db);
        if (!c->dt_avg_us) {
            c->dt_avg_us = (int32_t)dt;
        } else {                             /* one late reading barely moves it */
            const int32_t dtc = (int32_t)(dt < most ? dt : most);
            c->dt_avg_us += (dtc - c->dt_avg_us) / 8;
        }
    }

    if (!sd->learn_end_us) sd->learn_end_us = t_us + SD_LEARN_US;
    const bool learning = t_us < sd->learn_end_us;
    if (!(flags & SD_FLAG_SATURATED)) learn_rest(sd, channel, dt, learning);
    if (learning || !c->rest_primed) return false;   /* nothing rings while the rests settle */
    const float rise = c->smooth_db - c->rest_db;
    const uint32_t now_ms = (uint32_t)(t_us / 1000);
    const uint8_t bit = (uint8_t)(1u << channel);
    if (sd->settle_mask & bit) {
        /* Back from a long hold (refill): one median can sit 10 dB off the level (a VTX's FM swings
         * its band power), so pd follows the fresh average, ringing nothing, until it has settled
         * and starts again from there. A shorter hold meanwhile moved settle_from_us on (resume). */
        pd_resume(&sd->det, channel, rise, now_ms);
        if ((float)(t_us - sd->settle_from_us[channel]) >= SD_HOLD_SETTLE_TAUS * sd->tau_us)
            sd->settle_mask &= (uint8_t)~bit;
        return false;
    }
    return pd_update_w(&sd->det, channel, rise, 1.0f, now_ms, event);
}

/* Whether the guard treats this capture of `hop` as saturated: its own flag, or a clean capture
 * within SD_HOLD_TAIL_US of the last saturated one of a run that has lasted SD_HOLD_HOVER_US (a
 * drone hovering by the gate fades in and out of the rails; feeding the partner only in the fades'
 * gaps would sample it in bursts). Records the run. */
static bool hop_saturated(scan_detector_t *sd, unsigned hop, bool sat, int64_t t_us)
{
    if (hop >= SD_HOPS_MAX) return sat;
    int64_t *run = &sd->sat_run_us[hop], *last = &sd->sat_last_us[hop];
    if (sat) {
        if (!*last || t_us - *last > SD_HOLD_TAIL_US) {   /* a new run, no source known yet */
            *run = t_us;
            sd->sat_src[hop] = 0;
        }
        *last = t_us;
        return true;
    }
    return *last && t_us - *last <= SD_HOLD_TAIL_US && *last - *run >= SD_HOLD_HOVER_US;
}

/* The median of a channel's last 3 readings into sd_feed_hop; false until it has 3. */
static bool raw3(const scan_detector_t *sd, unsigned ch, int16_t *med)
{
    if (sd->raw_n[ch] < 3u) return false;
    const int16_t *h = sd->raw[ch];
    *med = median3(h[0], h[1], h[2]);
    return true;
}

/* Which reading of a guarded capture of two channels to hold: bit i for r[i] (see scan_detector.h).
 * A jump into the rails names its source for the rest of the run: the median of its last 3
 * readings stands more than SD_HOLD_DOM_DB over its partner's and more than SD_HOLD_JUMP_DB over its
 * own average (a drone flown in leads its average by 9 dB at most at 20 m/s, 16 at 60 m/s, host
 * model: the smoothed levels then hold the same channel). A held drone's average is frozen, so one
 * that crosses close during another's clipping run can be named partway through it: the clipping
 * drone is then held to the end (docs/detector.md). The products never pass for their source:
 * they top it by 12.8 dB at most on a capture (host DSP), so they would need two of the source's 3
 * readings over its median. Not the lowest of the 3: a clipping source's FM dips 6-22 dB on a
 * quarter of its readings, which kept it from being named for up to 18 captures. Otherwise the
 * smoothed levels decide. */
static unsigned pair_hold(scan_detector_t *sd, unsigned hop, unsigned ca, unsigned cb)
{
    const sd_channel_t *a = &sd->c[ca], *b = &sd->c[cb];
    if (hop < SD_HOPS_MAX) {
        int16_t med[2];
        if (raw3(sd, ca, &med[0]) && raw3(sd, cb, &med[1])) {
            const int32_t dom = (int32_t)(SD_HOLD_DOM_DB * QDB_PER_DB);
            for (unsigned i = 0; i < 2u; i++) {
                const sd_channel_t *s = i ? b : a;
                if (s->primed && (int32_t)med[i] > (int32_t)med[1u - i] + dom &&
                    (float)med[i] / QDB_PER_DB > s->smooth_db + SD_HOLD_JUMP_DB)
                    sd->sat_src[hop] = (uint8_t)(1u + i);
            }
        }
        if (sd->sat_src[hop]) return sd->sat_src[hop] == 1u ? 2u : 1u;   /* the source's partner */
    }
    if (!a->primed || !b->primed) return 0;               /* the smoothed levels, not these readings */
    if (a->smooth_db < b->smooth_db - SD_HOLD_DOM_DB) return 1u;
    if (b->smooth_db < a->smooth_db - SD_HOLD_DOM_DB) return 2u;
    return 0;
}

/* A held channel fed again: its median restarts on fresh readings. Once it is full (sd_feed_hop),
 * a real pass under way catches up, and a channel back from a long hold starts again from it. A
 * shorter hold while the channel still settles from a long one only pauses the settle: it moves
 * the settle's start on by the hold, so the average still gets SD_HOLD_SETTLE_TAUS of readings
 * (counted as long, one clipped capture every 200 ms restarted it for as long as they came). */
static void resume(scan_detector_t *sd, unsigned ch, int64_t t_us)
{
    sd_channel_t *c = &sd->c[ch];
    const uint8_t bit = (uint8_t)(1u << ch);
    sd->held_mask &= (uint8_t)~bit;
    c->nhist = 0;
    c->hidx = 0;
    const int64_t held_us = t_us - sd->held_since_us[ch];
    if (held_us > SD_HOLD_LONG_US) sd->long_mask |= bit;
    else if (sd->settle_mask & bit) sd->settle_from_us[ch] += held_us;
    else if (sd->held_rise_db[ch] >= sd->det.cfg.min_peak_db) sd->fresh_mask |= bit;
}

/* The reading that fills a resumed channel's fresh median (m, dB). Back from a long hold, the
 * average starts again at m and pd follows it until it has settled (sd_feed: SD_HOLD_SETTLE_TAUS),
 * then starts again from it (pd_resume), whatever the hold's length: held readings never reach pd,
 * which would otherwise keep its level from before the hold up to stale_ms and forget it past that.
 * Else a pass under way catches up to m less drop_db, upwards only, so a catch-up never rings by
 * itself. */
static void refill(scan_detector_t *sd, unsigned ch, float m, int64_t t_us)
{
    sd_channel_t *c = &sd->c[ch];
    const uint8_t bit = (uint8_t)(1u << ch);
    if (sd->long_mask & bit) {
        c->smooth_db = m;
        c->primed = true;
        sd->settle_mask |= bit;
        sd->settle_from_us[ch] = t_us;
    } else {
        const float up = m - sd->det.cfg.drop_db;
        if (c->primed && up > c->smooth_db) c->smooth_db = up;
    }
    sd->long_mask &= (uint8_t)~bit;
    sd->fresh_mask &= (uint8_t)~bit;
}

unsigned sd_feed_hop(scan_detector_t *sd, unsigned hop, const sd_reading_t *r, unsigned n, pd_event_t *ev,
                     unsigned *held)
{
    unsigned rang = 0, hold = 0;
    if (n > SD_HOP_READINGS) n = SD_HOP_READINGS;
    for (unsigned i = 0; i < n; i++) {                          /* every reading, held or not */
        const unsigned ch = r[i].channel;
        if (ch >= PD_CHANNELS || r[i].level_qdb == SD_LEVEL_NONE) continue;
        sd->raw[ch][sd->raw_i[ch]] = r[i].level_qdb;
        sd->raw_i[ch] = sd->raw_i[ch] >= 2u ? 0u : (uint8_t)(sd->raw_i[ch] + 1u);
        if (sd->raw_n[ch] < 3u) sd->raw_n[ch]++;
    }
    const bool sat = n && (r[0].flags & SD_FLAG_SATURATED);   /* one flag per capture */
    if (n == 2u && r[0].channel < PD_CHANNELS && r[1].channel < PD_CHANNELS && r[0].channel != r[1].channel &&
        hop_saturated(sd, hop, sat, r[0].t_us))
        hold = pair_hold(sd, hop, r[0].channel, r[1].channel);
    for (unsigned i = 0; i < n; i++) {
        const unsigned ch = r[i].channel;
        if (ch >= PD_CHANNELS || r[i].level_qdb == SD_LEVEL_NONE) continue;   /* sd_feed ignores these too */
        const uint8_t bit = (uint8_t)(1u << ch);
        if (hold & (1u << i)) {
            if (!(sd->held_mask & bit)) {
                sd->held_since_us[ch] = r[i].t_us;
                sd->held_rise_db[ch] = sd->det.ch[ch].primed ? sd->det.ch[ch].smooth : 0.0f;
            }
            sd->held_mask |= bit;
            sd->held[ch]++;
            continue;
        }
        if (sd->held_mask & bit) resume(sd, ch, r[i].t_us);
        sd_channel_t *c = &sd->c[ch];
        if (((sd->fresh_mask | sd->long_mask) & bit) && c->nhist == 2u)   /* this reading fills the fresh median */
            refill(sd, ch, (float)median3(c->hist[0], c->hist[1], r[i].level_qdb) / QDB_PER_DB, r[i].t_us);
        pd_event_t e;
        if (sd_feed(sd, ch, r[i].level_qdb, r[i].flags, r[i].t_us, &e)) {
            if (ev) ev[i] = e;
            rang |= 1u << i;
        }
    }
    if (held) *held = hold;
    return rang;
}

void sd_rotation(scan_detector_t *sd, int64_t t_us)
{
    float v[PD_CHANNELS];
    unsigned n = 0;
    for (unsigned k = 0; k < PD_CHANNELS; k++) {
        if (!sd->c[k].rest_primed) continue;
        unsigned i = n++;
        for (; i && v[i - 1u] > sd->c[k].rest_db; i--) v[i] = v[i - 1u];
        v[i] = sd->c[k].rest_db;
    }
    sd->cap_db = n ? v[n / 2u] + SD_REST_SPREAD_DB : INFINITY;
    pd_expire(&sd->det, (uint32_t)(t_us / 1000));
}

bool sd_learning(const scan_detector_t *sd, int64_t t_us)
{
    return !sd->learn_end_us || t_us < sd->learn_end_us;
}

float sd_rise_db(const scan_detector_t *sd, unsigned channel)
{
    return channel < PD_CHANNELS ? sd->det.ch[channel].smooth : 0.0f;
}

unsigned sd_strongest(const scan_detector_t *sd)
{
    unsigned top = 0;
    for (unsigned k = 1; k < PD_CHANNELS; k++)
        if (sd->det.ch[k].smooth > sd->det.ch[top].smooth) top = k;
    return top;
}

float sd_tau_ms(const scan_detector_t *sd) { return sd->tau_us / 1000.0f; }

float sd_delay_ms(const scan_detector_t *sd, unsigned channel)
{
    return channel < PD_CHANNELS ? (sd->tau_us + (float)sd->c[channel].dt_avg_us) / 1000.0f : 0.0f;
}
