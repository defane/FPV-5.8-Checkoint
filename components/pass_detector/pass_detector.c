/* Checkpoint pass detector (pass_detector.h). */
#include "pass_detector.h"
#include <string.h>

static bool reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }

pd_config_t pd_config_default(void)
{
    return (pd_config_t){
        .alpha = 0.3f,
        .active_db = 15.0f,
        .hysteresis_db = 3.0f,
        .drop_db = 3.0f,
        .min_peak_db = 25.0f,
        .refractory_ms = 1500u,
        .stale_ms = 1000u,
    };
}

bool pd_config_valid(const pd_config_t *c)
{
    return c && c->alpha > 0.0f && c->alpha <= 1.0f && c->hysteresis_db > 0.0f && c->drop_db > 0.0f &&
           c->min_peak_db >= c->active_db && c->stale_ms > 0u;
}

void pd_reset_channel(pass_detector_t *d, unsigned channel)
{
    if (channel < PD_CHANNELS) memset(&d->ch[channel], 0, sizeof(d->ch[channel]));
}

void pd_reset_all(pass_detector_t *d) { memset(d->ch, 0, sizeof(d->ch)); }

void pd_init(pass_detector_t *d, const pd_config_t *cfg)
{
    d->cfg = pd_config_valid(cfg) ? *cfg : pd_config_default();
    d->rearm_db = 0.0f;
    pd_reset_all(d);
}

void pd_set_rearm(pass_detector_t *d, float drop_db) { d->rearm_db = drop_db > 0.0f ? drop_db : 0.0f; }

bool pd_set_config(pass_detector_t *d, const pd_config_t *cfg)
{
    if (!pd_config_valid(cfg)) return false;
    d->cfg = *cfg;
    return true;
}

/* Lost the channel: forget the smoothing and any pass in progress, but a
 * pass that already rang keeps its refractory period. */
static void go_stale(pd_channel_t *c)
{
    const bool cooling = c->state == PD_COOLDOWN || c->state == PD_PEAK_PASSED;
    const uint32_t until = c->cooldown_until_ms;
    memset(c, 0, sizeof(*c));
    if (cooling) {
        c->state = PD_COOLDOWN;
        c->cooldown_until_ms = until;
    }
}

static bool stale(const pass_detector_t *d, const pd_channel_t *c, uint32_t now_ms)
{
    return c->primed && now_ms - c->last_ms > d->cfg.stale_ms;
}

void pd_expire(pass_detector_t *d, uint32_t now_ms)
{
    for (unsigned k = 0; k < PD_CHANNELS; ++k)
        if (stale(d, &d->ch[k], now_ms)) go_stale(&d->ch[k]);
}

bool pd_update(pass_detector_t *d, unsigned channel, float level_db, uint32_t now_ms, pd_event_t *event)
{
    return pd_update_w(d, channel, level_db, d->cfg.alpha, now_ms, event);
}

bool pd_update_w(pass_detector_t *d, unsigned channel, float level_db, float weight, uint32_t now_ms,
                 pd_event_t *event)
{
    if (channel >= PD_CHANNELS || level_db != level_db) return false;   /* NaN: no sample */
    const pd_config_t *cfg = &d->cfg;
    pd_channel_t *c = &d->ch[channel];
    if (stale(d, c, now_ms)) go_stale(c);
    const uint32_t dt_ms = now_ms - c->last_ms;
    /* weight 1 is exact: a + 1 * (b - a) is not always b in float */
    c->smooth = c->primed && weight < 1.0f ? c->smooth + weight * (level_db - c->smooth) : level_db;
    if (!c->primed) {
        c->slow = c->smooth;
    } else {                                 /* time-weighted: the same average at any rate */
        const float w = (float)dt_ms / (float)PD_REARM_TAU_MS;
        c->slow += (w < 1.0f ? w : 1.0f) * (c->smooth - c->slow);
    }
    c->primed = true;
    c->last_ms = now_ms;
    const float release = cfg->active_db - cfg->hysteresis_db;
    const float rearm = d->rearm_db;

    switch (c->state) {
    case PD_IDLE: {
        if (c->rel) {
            if (c->smooth < release) c->rel = false;   /* armed as without the relative re-arm */
            else if (c->slow < c->trough) c->trough = c->slow;
        }
        /* after a relative re-arm a pass must stand out of the trough, not just above active_db */
        const float rise = c->rel && c->trough + rearm > cfg->active_db ? c->trough + rearm : cfg->active_db;
        if (c->smooth > rise) {
            c->state = PD_RISING;
            c->peak = c->smooth;
            c->peak_ms = now_ms;
        }
        return false;
    }
    case PD_RISING:
        if (c->smooth > c->peak) {
            c->peak = c->smooth;
            c->peak_ms = now_ms;
        } else if (c->peak - c->smooth >= cfg->drop_db && c->peak >= cfg->min_peak_db) {
            c->state = PD_PEAK_PASSED;
            c->cooldown_until_ms = now_ms + cfg->refractory_ms;
            if (event) *event = (pd_event_t){(uint8_t)channel, c->peak, c->peak_ms, now_ms, c->rel};
            c->rel = false;
            return true;
        } else if (c->smooth < release) {
            c->state = PD_IDLE;   /* a far drone: never reached min_peak_db */
            c->rel = false;
        } else if (c->rel && c->smooth < c->trough + rearm - cfg->hysteresis_db) {
            c->state = PD_IDLE;   /* the same, out of a trough */
        }
        return false;
    case PD_PEAK_PASSED:
        c->state = PD_COOLDOWN;
        /* fall through */
    case PD_COOLDOWN:
        if (rearm > 0.0f && c->smooth > c->peak) c->peak = c->smooth;   /* the highest level since the pass */
        if (reached(now_ms, c->cooldown_until_ms)) {
            if (c->smooth < release) {
                c->state = PD_IDLE;
            } else if (rearm > 0.0f && c->slow <= c->peak - rearm) {
                c->state = PD_IDLE;   /* the drone has left, but not under the release */
                c->rel = true;
                c->trough = c->slow;
            }
        }
        return false;
    }
    return false;
}

const char *pd_state_name(pd_state_t state)
{
    switch (state) {
    case PD_IDLE: return "IDLE";
    case PD_RISING: return "RISING";
    case PD_PEAK_PASSED: return "PEAK_PASSED";
    case PD_COOLDOWN: return "COOLDOWN";
    }
    return "?";
}
