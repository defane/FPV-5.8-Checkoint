/* race_timing: lap/sector timing. See race_timing.h. */
#include "race_timing.h"
#include <stddef.h>

void rt_init(race_timing_t *rt, const rt_config_t *cfg) {
    for (unsigned i = 0; i < sizeof(*rt); i++) ((uint8_t *)rt)[i] = 0;
    uint8_t g = cfg ? cfg->num_gates : 1u;
    if (g < 1u) g = 1u;
    if (g > RT_MAX_GATES) g = RT_MAX_GATES;
    rt->cfg.num_gates = g;
}

void rt_reset(race_timing_t *rt) {
    const rt_config_t keep = rt->cfg;
    rt_init(rt, &keep);
}

static rt_channel_t *slot_for(race_timing_t *rt, uint8_t channel) {
    for (unsigned i = 0; i < rt->num_channels; i++)
        if (rt->ch[i].active && rt->ch[i].channel == channel) return &rt->ch[i];
    if (rt->num_channels >= RT_MAX_CHANNELS) return NULL;
    rt_channel_t *c = &rt->ch[rt->num_channels++];
    c->active = true;
    c->channel = channel;
    c->laps = 0;
    c->last_lap_ms = 0;
    c->best_lap_ms = 0;
    c->last_gate = -1;
    c->last_cross_ms = 0;
    for (unsigned k = 0; k < RT_MAX_GATES; k++) { c->gate_seen[k] = false; c->gate_ms[k] = 0; }
    return c;
}

static void clear_lap(rt_channel_t *c) {
    for (unsigned k = 0; k < RT_MAX_GATES; k++) { c->gate_seen[k] = false; c->gate_ms[k] = 0; }
}

bool rt_cross(race_timing_t *rt, uint8_t gate, uint8_t channel,
              uint32_t fleet_ms, uint32_t *lap_ms_out) {
    if (gate >= rt->cfg.num_gates) return false;
    rt_channel_t *c = slot_for(rt, channel);
    if (!c) return false;

    c->last_gate = (int16_t)gate;
    c->last_cross_ms = fleet_ms;

    if (gate == 0u) {
        bool completed = false;
        if (c->gate_seen[0]) {                 /* a lap was in progress */
            const uint32_t lap = fleet_ms - c->gate_ms[0];
            c->last_lap_ms = lap;
            if (c->best_lap_ms == 0u || lap < c->best_lap_ms) c->best_lap_ms = lap;
            c->laps++;
            if (lap_ms_out) *lap_ms_out = lap;
            completed = true;
        }
        clear_lap(c);                          /* open the next lap */
        c->gate_seen[0] = true;
        c->gate_ms[0] = fleet_ms;
        return completed;
    }

    c->gate_seen[gate] = true;
    c->gate_ms[gate] = fleet_ms;
    return false;
}

uint32_t rt_sector_ms(const race_timing_t *rt, uint8_t channel, uint8_t gate) {
    if (gate == 0u || gate >= rt->cfg.num_gates) return 0u;
    const rt_channel_t *c = rt_channel(rt, channel);
    if (!c) return 0u;
    if (!c->gate_seen[gate] || !c->gate_seen[gate - 1u]) return 0u;
    return c->gate_ms[gate] - c->gate_ms[gate - 1u];
}

const rt_channel_t *rt_channel(const race_timing_t *rt, uint8_t channel) {
    for (unsigned i = 0; i < rt->num_channels; i++)
        if (rt->ch[i].active && rt->ch[i].channel == channel) return &rt->ch[i];
    return NULL;
}
