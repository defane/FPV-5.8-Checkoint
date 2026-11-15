/* Scan detector. See scan_detector.h. */
#include "scan_detector.h"
#include <math.h>
#include <string.h>

#define QDB_PER_DB 4.0f   /* level codes per dB (= SDR_QDB_PER_DB) */

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

/* The rest follows the low smoothed values; never from a saturated capture. */
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
    else if (sd->det.ch[ch].state == PD_IDLE) c->rest_db += SD_REST_UP_DB * s;
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
    return pd_update_w(&sd->det, channel, c->smooth_db - c->rest_db, 1.0f, (uint32_t)(t_us / 1000), event);
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
