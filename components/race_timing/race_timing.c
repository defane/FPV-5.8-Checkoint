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

bool rt_reset_channel(race_timing_t *rt, uint8_t channel) {
    for (unsigned i = 0; i < rt->num_channels; i++) {
        rt_channel_t *c = &rt->ch[i];
        if (!c->active || c->channel != channel) continue;
        c->laps = 0;
        c->last_lap_ms = 0;
        c->best_lap_ms = 0;
        c->last_gate = -1;
        for (unsigned k = 0; k < RT_MAX_GATES; k++) { c->gate_seen[k] = false; c->gate_ms[k] = 0; }
        return true;
    }
    return false;
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
            rt_lap_t *r = &c->recent[(c->laps - 1u) % RT_HISTORY];
            r->number = c->laps;
            r->lap_ms = lap;
            for (unsigned k = 0; k < RT_MAX_GATES; k++)
                r->split_ms[k] = (k && c->gate_seen[k]) ? c->gate_ms[k] - c->gate_ms[0] : 0u;
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

const rt_lap_t *rt_recent_lap(const rt_channel_t *c, unsigned age) {
    if (age >= RT_HISTORY || age >= c->laps) return NULL;
    return &c->recent[(c->laps - 1u - age) % RT_HISTORY];
}

uint32_t rt_clock_to_fleet(rt_clock_t *clk, uint32_t local_ms, uint32_t fleet_now_ms) {
    const uint32_t offset = fleet_now_ms - local_ms;
    /* A timestamp only arrives late, so the smallest offset is the closest to the true
     * one. A clock going backwards is a board that rebooted: learn it again. A constant
     * offset leaves the time between two local crossings, a lap, exactly as measured. */
    if (!clk->valid || (int32_t)(local_ms - clk->last_ms) < 0 || (int32_t)(offset - clk->offset_ms) < 0)
        clk->offset_ms = offset;
    clk->valid = true;
    clk->last_ms = local_ms;
    return local_ms + clk->offset_ms;
}
