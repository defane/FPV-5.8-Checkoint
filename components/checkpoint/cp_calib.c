/* The passage calibration's session: with a drone in the passage, then 2 m away, the scan task
 * samples every channel's smoothed rise for LP_CALIB_SECS seconds into a histogram (heap, only
 * while it measures) and pass_calib turns the two measurements into proposed thresholds. The
 * bridge's web reads the session through CALIB_STATE and applies the thresholds itself
 * (CONFIG_SET); nothing here changes the detector. It never blinds the gate: the drone's VTX is
 * ON, and a pass it rings during step 1 is a real one (sound and a lap on the master). */
#include "cp_internal.h"
#include <stdlib.h>
#include <string.h>

#define US_PER_S 1000000

static void publish(cp_t *cp, const lp_calib_t *st)
{
    portENTER_CRITICAL(&cp_mux);
    cp->calib.st = *st;
    portEXIT_CRITICAL(&cp_mux);
}

static void end_session(cp_t *cp)
{
    cp_calib_t *c = &cp->calib;
    free(c->hist);
    c->hist = NULL;
    c->measuring = 0;
    c->active = false;
    lp_calib_t none = { 0 };
    publish(cp, &none);
}

void cp_calib_request(cp_t *cp, uint8_t step, uint8_t channel)
{
    if (step > 2u) return;
    portENTER_CRITICAL(&cp_mux);
    cp->calib.req_ch = channel < PC_CHANNELS ? channel : LP_CALIB_ANY;
    cp->calib.req = step == 0 ? 3u : step;
    portEXIT_CRITICAL(&cp_mux);
}

/* Is it a moment to measure? The rests are not being learnt, no calibration, bench or gain change. */
static bool can_measure(const cp_t *cp, int64_t now_us)
{
    return cp->live && !cp->engine_failed && !sd_learning(&cp->sd, now_us) && !sdr_rx_calibrating() &&
           !cp->bench_running && !cp->bench_req && cp->recal_req < 0 && !cp->gain_dirty && !cp->relearn_pending;
}

static void start(cp_t *cp, uint8_t step, uint8_t channel, int64_t now_us)
{
    cp_calib_t *c = &cp->calib;
    lp_calib_t st = c->st;
    const bool have1 = c->active && (st.phase == LP_CALIB_MEASURED1 || st.phase == LP_CALIB_DONE);
    if (c->measuring || !can_measure(cp, now_us) || (step == 2 && !have1)) {
        st.flags |= PC_BUSY;                  /* refused: the page says so; the session goes on as it was */
        if (!c->active) st.phase = LP_CALIB_NONE;
        publish(cp, &st);
        return;
    }
    if (!c->hist) c->hist = malloc(sizeof(*c->hist));
    if (!c->hist) {
        st.flags |= PC_BUSY;
        publish(cp, &st);
        return;
    }
    pc_hist_clear(c->hist);
    if (step == 1) c->want_ch = channel;
    if (step == 1) memset(&st, 0, sizeof st);
    else { memset(st.away, 0, sizeof st.away); st.active_db_x10 = st.hysteresis_db_x10 = st.drop_db_x10 = st.min_peak_db_x10 = 0; st.gain_steps = 0; }
    if (step == 1) st.channel = 255;
    st.flags = 0;
    st.phase = step == 1 ? LP_CALIB_MEASURING1 : LP_CALIB_MEASURING2;
    st.secs_left = LP_CALIB_SECS;
    c->active = true;
    c->measuring = step;
    c->end_us = now_us + (int64_t)LP_CALIB_SECS * US_PER_S;
    c->idle_us = c->end_us + (int64_t)LP_CALIB_IDLE_S * US_PER_S;
    publish(cp, &st);
}

void cp_calib_apply(cp_t *cp, int64_t now_us)
{
    cp_calib_t *c = &cp->calib;
    uint8_t req, ch;
    portENTER_CRITICAL(&cp_mux);
    req = c->req;
    ch = c->req_ch;
    c->req = 0;
    portEXIT_CRITICAL(&cp_mux);
    if (req == 3u) { end_session(cp); return; }
    if (req == 1u || req == 2u) { start(cp, req, ch, now_us); return; }
    if (c->active && !c->measuring && now_us > c->idle_us) end_session(cp);   /* nobody came back */
}

void cp_calib_capture(cp_t *cp, bool saturated)
{
    if (cp->calib.measuring) pc_hist_capture(cp->calib.hist, saturated);
}

static void finish(cp_t *cp)
{
    cp_calib_t *c = &cp->calib;
    lp_calib_t st = c->st;
    pc_stats_t s;
    if (c->measuring == 1) {
        const unsigned top = pc_strongest(c->hist);
        const unsigned ch = c->want_ch < PC_CHANNELS ? c->want_ch : top;   /* the channel asked, else the one standing highest */
        pc_stats(c->hist, ch, &c->gate);
        st.channel = (uint8_t)ch;
        st.gate[0] = c->gate.p10; st.gate[1] = c->gate.p50; st.gate[2] = c->gate.p90; st.gate[3] = c->gate.p95; st.gate[4] = c->gate.max;
        st.gate_sat_pct = c->gate.sat_pct;
        st.phase = LP_CALIB_MEASURED1;
        st.flags = c->gate.p95 < (int)(2.0f * PC_SEEN_DB) ? PC_NO_DRONE : 0u;
        if (c->gate.sat_pct >= PC_CLIP_PCT) st.flags |= PC_CLIPPING;
        if (top != ch) {                      /* a channel was asked and another stands 6 dB higher: the drone is elsewhere */
            pc_stats(c->hist, top, &s);
            if (s.p95 >= c->gate.p95 + 12) st.flags |= PC_OTHER_CHANNEL;
        }
    } else {
        pc_stats_t top;
        pc_result_t r;
        pc_stats(c->hist, st.channel, &s);
        pc_stats(c->hist, pc_strongest(c->hist), &top);
        pc_solve(&c->gate, &s, &top, &r);
        st.away[0] = s.p10; st.away[1] = s.p50; st.away[2] = s.p90; st.away[3] = s.p95; st.away[4] = s.max;
        st.active_db_x10 = r.active_db_x10; st.hysteresis_db_x10 = r.hysteresis_db_x10;
        st.drop_db_x10 = r.drop_db_x10; st.min_peak_db_x10 = r.min_peak_db_x10;
        st.gain_steps = r.gain_steps;
        st.flags = r.flags;
        st.phase = LP_CALIB_DONE;
    }
    st.secs_left = 0;
    free(c->hist);
    c->hist = NULL;
    c->measuring = 0;
    publish(cp, &st);
}

void cp_calib_rotation(cp_t *cp, int64_t now_us)
{
    cp_calib_t *c = &cp->calib;
    if (!c->measuring) return;
    for (unsigned ch = 0; ch < PC_CHANNELS; ch++) pc_hist_add(c->hist, ch, sd_rise_db(&cp->sd, ch));
    if (now_us >= c->end_us) { finish(cp); return; }
    const uint8_t left = (uint8_t)((c->end_us - now_us + US_PER_S - 1) / US_PER_S);
    if (left != c->st.secs_left) {
        lp_calib_t st = c->st;
        st.secs_left = left;
        publish(cp, &st);
    }
}

bool cp_calib_get(cp_t *cp, lp_calib_t *out)
{
    portENTER_CRITICAL(&cp_mux);
    *out = cp->calib.st;
    portEXIT_CRITICAL(&cp_mux);
    return out->phase != LP_CALIB_NONE;
}
