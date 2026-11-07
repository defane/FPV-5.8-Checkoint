/* Scan detector. See scan_detector.h. */
#include "scan_detector.h"
#include <math.h>

pd_config_t sd_config_default(void)
{
    return (pd_config_t){.alpha = 0.25f, .active_db = 8.0f, .hysteresis_db = 3.0f, .drop_db = 3.0f,
                         .min_peak_db = 10.0f, .refractory_ms = 1500u, .stale_ms = 1000u};
}

void sd_init(scan_detector_t *sd, const pd_config_t *cfg)
{
    *sd = (scan_detector_t){0};
    pd_init(&sd->det, cfg);
}

float sd_power_db(uint32_t power)
{
    if (!power) return SD_FLOOR_DB;
    const float db = 10.0f * log10f((float)power / 256.0f);
    return db > SD_FLOOR_DB ? db : SD_FLOOR_DB;
}

static float median_rest(const scan_detector_t *sd)
{
    float v[PD_CHANNELS];
    unsigned n = 0;
    for (unsigned k = 0; k < PD_CHANNELS; k++) {
        if (!sd->primed[k]) continue;
        unsigned i = n++;
        for (; i && v[i - 1u] > sd->rest_db[k]; i--) v[i] = v[i - 1u];
        v[i] = sd->rest_db[k];
    }
    return n ? v[n / 2u] : 0.0f;
}

static void learn_rest(scan_detector_t *sd, unsigned ch, float db, float cap)
{
    if (!sd->primed[ch]) {
        sd->primed[ch] = true;
        sd->rest_db[ch] = db;
        return;
    }
    const float rate = sd->sweeps < SD_LEARN_SWEEPS ? SD_LEARN_RATE : 1.0f;
    if (db < sd->rest_db[ch]) sd->rest_db[ch] -= SD_REST_DOWN_DB * rate;
    else if (sd->det.ch[ch].state == PD_IDLE) sd->rest_db[ch] += SD_REST_UP_DB * rate;
    if (sd->rest_db[ch] > cap) sd->rest_db[ch] = cap;
}

unsigned sd_sweep(scan_detector_t *sd, const uint16_t power[PD_CHANNELS], uint32_t now_ms, pd_event_t *events)
{
    const float cap = median_rest(sd) + SD_REST_SPREAD_DB;
    unsigned n = 0;
    for (unsigned ch = 0; ch < PD_CHANNELS; ch++) {
        if (power[ch] == SD_NO_DATA) continue;
        const float db = sd_power_db(power[ch]);
        const bool first = !sd->primed[ch];
        learn_rest(sd, ch, db, first ? db : cap);
        if (sd->sweeps < SD_LEARN_SWEEPS) continue;
        float rise = db - sd->rest_db[ch];
        const float signal = db - SD_SIGNAL_DB + sd->det.cfg.active_db;   /* active only from SD_SIGNAL_DB up */
        if (rise > signal) rise = signal;
        if (pd_update(&sd->det, ch, rise, now_ms, &events[n])) n++;
    }
    sd->sweeps++;
    return n;
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
