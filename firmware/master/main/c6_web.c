#include "c6_web.h"
#include "admin_hash.h"
#include "c6_hub.h"
#include "c6_link.h"
#include "c6_store.h"
#include "fleet_proto.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "web_auth_esp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "c6_web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t get_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

/* Snapshots for the handlers: too large for the httpd stack (c6_fleet_t is
 * ~1.5 KB), and httpd runs one handler at a time. */
static c6_snapshot_t s_snap;
static c6_fleet_t    s_fleet;
static race_timing_t s_results;

/* A C5's firmware id is foreign text: keep it inert in the JSON and in the
 * page's HTML (printable ASCII minus quotes, backslash and markup). */
static const char *safe_fw(const char *fw, char out[LP_FW_LEN]) {
    unsigned i = 0;
    for (; i + 1u < LP_FW_LEN && fw[i]; i++)
        out[i] = (fw[i] >= 0x20 && fw[i] < 0x7f && !strchr("\"\\<>&'", fw[i])) ? fw[i] : '?';
    out[i] = '\0';
    return out;
}

/* The C5 health fields shared by "local" and each node, and the re-arm drop its C5 echoed
 * (rearm_x10, FP_REARM_UNKNOWN: -1, none yet), set against the fleet's on the page. */
static int put_health(char *b, size_t cap, uint32_t passes, uint8_t flags, uint8_t gain,
                      uint32_t uptime_s, uint32_t heap_min, uint32_t faults, const char *fw,
                      uint16_t rearm_x10) {
    char f[LP_FW_LEN];
    return snprintf(b, cap, "\"passes\":%lu,\"cal_needed\":%s,\"calibrating\":%s,\"gain\":%u,"
                    "\"c5_uptime_s\":%lu,\"heap_min\":%lu,\"faults\":%lu,\"fw\":\"%s\",\"rearm_db_x10\":%d",
                    (unsigned long)passes, (flags & LP_HEALTH_CAL_NEEDED) ? "true" : "false",
                    (flags & LP_HEALTH_CALIBRATING) ? "true" : "false", gain,
                    (unsigned long)uptime_s, (unsigned long)heap_min, (unsigned long)faults,
                    safe_fw(fw, f), rearm_x10 == FP_REARM_UNKNOWN ? -1 : (int)rearm_x10);
}

/* Gate 0's echoed drop as put_health takes it: FP_REARM_UNKNOWN until its C5's first CONFIG_STATE. */
static uint16_t rearm_echo(bool known, int32_t x10) {
    return known && x10 >= 0 && x10 < (int32_t)FP_REARM_UNKNOWN ? (uint16_t)x10 : (uint16_t)FP_REARM_UNKNOWN;
}

/* One level per Raceband channel, dB x10, LP_CN_UNKNOWN when not measured. */
static int put_levels(char *b, size_t cap, const int8_t *levels) {
    int o = snprintf(b, cap, "\"levels\":[");
    for (unsigned ch = 0; ch < LP_LEVELS; ch++) {
        const bool known = levels && levels[ch] != LP_LEVEL_UNKNOWN;
        o += snprintf(b + o, cap - o, "%s%d", ch ? "," : "", known ? levels[ch] * 10 : LP_CN_UNKNOWN);
    }
    return o + snprintf(b + o, cap - o, "],");
}

#define STATES_LEN 16u   /* "3,3,3,3,3,3,3,3" + NUL */

/* Each channel's detector state (pd_state_t, 0..3), R1 first, for a "states":[...]. */
static const char *put_states(char out[STATES_LEN], uint16_t states) {
    int o = 0;
    for (unsigned ch = 0; ch < LP_LEVELS; ch++)
        o += snprintf(out + o, STATES_LEN - o, "%s%u", ch ? "," : "", lp_state_of(states, ch));
    return out;
}

static esp_err_t get_state(httpd_req_t *req) {
    c6_store_snapshot(&s_snap);
    c6_hub_snapshot(&s_fleet);
    const c6_snapshot_t *s = &s_snap;
    const c6_fleet_t *f = &s_fleet;

    /* Worst case (every field at its widest, every list full) 10847 B: header + local 487 (+20 for pairing_left_s) (its
     * states 27 and levels 59, the fleet's and gate 0's re-arm drop), 15 nodes 7544 (+16 each for "custom": 487 + 16 with
     * its comma, states and levels included), 8 standings 1031, 24 passes 1727, the lists'
     * brackets 38; sized so it never truncates. */
    static char body[11008];   /* larger than the stack; this handler is serialised by httpd */
    const lp_stats_t *l = &s->local;
    char states[STATES_LEN];
    int o = 0;
    o += snprintf(body + o, sizeof body - o,
                  "{\"pairing\":%s,\"pairing_left_s\":%u,\"num_gates\":%u,\"has_local\":%s,"
                  "\"local\":{\"cn_x10\":%ld,\"states\":[%s],",
                  f->pairing ? "true" : "false", f->pairing_left_s, f->num_gates, s->has_local ? "true" : "false",
                  (long)l->cn_x10, put_states(states, s->has_local ? l->states : 0u));
    o += put_levels(body + o, sizeof body - o, s->has_local ? l->levels : NULL);
    o += put_health(body + o, sizeof body - o, l->passes, l->flags, l->gain, l->uptime_s,
                    l->heap_min, l->faults, l->fw, rearm_echo(s->has_local_cfg, s->local_cfg.rearm_db_x10));
    o += snprintf(body + o, sizeof body - o,
                  "},\"total_passes\":%lu,\"volume\":%u,\"rearm_db_x10\":%u,"
                  "\"uptime_ms\":%lu,\"free_heap\":%lu,\"heap_total\":%lu,",
                  (unsigned long)s->total_passes, (unsigned)f->volume, (unsigned)s->rearm_x10,
                  (unsigned long)f->uptime_ms, (unsigned long)f->free_heap,
                  (unsigned long)f->heap_total);

    o += snprintf(body + o, sizeof body - o, "\"nodes\":[");
    for (unsigned i = 0; i < f->num_nodes && o < (int)sizeof body; i++) {
        const c6_node_t *n = &f->nodes[i];
        o += snprintf(body + o, sizeof body - o,
                      "%s{\"node_id\":%u,\"gate\":%u,\"channel\":%u,\"online\":%s,\"cn_x10\":%ld,"
                      "\"states\":[%s],\"last_seen_ms\":%lu,\"rssi\":%d,\"rx_frames\":%lu,"
                      "\"status_rx\":%lu,\"status_lost\":%lu,\"custom\":%s,"
                      "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",",
                      i ? "," : "", n->node_id, n->gate_index, n->channel,
                      n->online ? "true" : "false", (long)n->cn_x10,
                      put_states(states, n->online ? n->states : 0u), (unsigned long)n->last_seen_ms,
                      (int)n->rssi, (unsigned long)n->rx_frames,
                      (unsigned long)n->status_rx, (unsigned long)n->status_lost, n->custom ? "true" : "false",
                      n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
        o += put_levels(body + o, sizeof body - o, n->online ? n->levels : NULL);
        o += put_health(body + o, sizeof body - o, n->passes, n->flags, n->gain, n->c5_uptime_s,
                        n->heap_min, n->faults, n->fw, n->rearm_x10);
        o += snprintf(body + o, sizeof body - o, "}");
    }
    o += snprintf(body + o, sizeof body - o, "],\"standings\":[");
    for (unsigned i = 0; i < f->num_standings && o < (int)sizeof body; i++) {
        const c6_standing_t *st = &f->standings[i];
        o += snprintf(body + o, sizeof body - o,
                      "%s{\"channel\":%u,\"laps\":%lu,\"last_lap_ms\":%lu,\"best_lap_ms\":%lu,\"last_gate\":%d,"
                      "\"last_seen_ms\":%lu}",
                      i ? "," : "", st->channel, (unsigned long)st->laps,
                      (unsigned long)st->last_lap_ms, (unsigned long)st->best_lap_ms, st->last_gate,
                      (unsigned long)st->last_seen_ms);
    }
    o += snprintf(body + o, sizeof body - o, "],\"passes\":[");
    for (unsigned i = 0; i < s->n_passes && o < (int)sizeof body; i++)
        o += snprintf(body + o, sizeof body - o,
                      "%s{\"gate\":%u,\"channel\":%u,\"peak_db_x10\":%ld,\"at_ms\":%lu}",
                      i ? "," : "", s->passes[i].gate, s->passes[i].channel,
                      (long)s->passes[i].peak_db_x10, (unsigned long)s->passes[i].at_ms);
    o += snprintf(body + o, sizeof body - o, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, o);
}

/* Each channel's last laps, a chunk per channel: 8 full ones reach 18 KB. */
static esp_err_t get_results(httpd_req_t *req) {
    c6_hub_results(&s_results);
    const race_timing_t *rt = &s_results;
    /* Worst chunk (16 gates, every number at its widest) 2244 B: header 28, channel 24,
     * 10 laps x 219, closing 2; sized so it never truncates. */
    static char body[2304];   /* larger than the stack; this handler is serialised by httpd */
    httpd_resp_set_type(req, "application/json");
    int o = snprintf(body, sizeof body, "{\"num_gates\":%u,\"channels\":[", rt->cfg.num_gates);
    for (unsigned i = 0; i < rt->num_channels; i++) {
        const rt_channel_t *c = &rt->ch[i];
        o += snprintf(body + o, sizeof body - o, "%s{\"channel\":%u,\"laps\":[", i ? "," : "", c->channel);
        const rt_lap_t *lap;
        for (unsigned age = 0; (lap = rt_recent_lap(c, age)) != NULL; age++) {
            o += snprintf(body + o, sizeof body - o, "%s{\"number\":%lu,\"lap_ms\":%lu,\"splits\":[",
                          age ? "," : "", (unsigned long)lap->number, (unsigned long)lap->lap_ms);
            for (unsigned g = 1; g < rt->cfg.num_gates; g++)
                o += snprintf(body + o, sizeof body - o, "%s%lu", g > 1 ? "," : "", (unsigned long)lap->split_ms[g]);
            o += snprintf(body + o, sizeof body - o, "]}");
        }
        o += snprintf(body + o, sizeof body - o, "]}");
        if (httpd_resp_send_chunk(req, body, o) != ESP_OK) return ESP_FAIL;
        o = 0;
    }
    o += snprintf(body + o, sizeof body - o, "]}");
    if (httpd_resp_send_chunk(req, body, o) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Tiny parser for the flat integer objects our own client POSTs. */
static int32_t json_i(const char *body, const char *key, int32_t def) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return def;
    p = strchr(p + strlen(pat), ':');
    if (!p) return def;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end;
    const long v = strtol(p, &end, 10);
    return end == p ? def : (int32_t)v;
}

/* Same for a "key":"value" string; false if absent or longer than out[cap]. */
static bool json_s(const char *body, const char *key, char *out, size_t cap) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':'))) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '"') return false;
    size_t n = 0;
    while (*p && *p != '"' && n + 1u < cap) out[n++] = *p++;
    out[n] = '\0';
    return *p == '"';
}

static esp_err_t ok_json(httpd_req_t *req, bool ok) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* ---- admin auth (spectators are read-only; admin actions are signed) ---- */
/* Every admin request is signed in the page with K = PBKDF2(password, ADMIN_SALT,
 * ADMIN_ITERATIONS), the only form of the password the firmware holds (admin_hash.h,
 * generated at build time by web_auth_admin_key()); components/web_auth checks the signature,
 * the session and its replay counter. web_auth_request() also reads the body:
 * on false its 400/401 is already sent. */
static const uint8_t s_admin_key[] = ADMIN_KEY;

static esp_err_t get_auth(httpd_req_t *req) {
    char buf[8];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"admin\":true}");
}

static esp_err_t post_pairing(httpd_req_t *req) {
    char buf[128];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    c6_hub_set_pairing(json_i(buf, "open", 0) != 0);
    return ok_json(req, true);
}

static esp_err_t post_node(httpd_req_t *req) {
    char buf[128];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    bool ok = c6_hub_set_node((uint8_t)json_i(buf, "node_id", -1),
                              (uint8_t)json_i(buf, "gate", 0),
                              (uint8_t)json_i(buf, "channel", 0));
    return ok_json(req, ok);
}

static esp_err_t post_node_remove(httpd_req_t *req) {
    char buf[128];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    return ok_json(req, c6_hub_remove_node((uint8_t)json_i(buf, "node_id", -1)));
}

static esp_err_t post_reset(httpd_req_t *req) {
    char buf[16];   /* "{}" for the race, {"channel":255} for one: signed like any body */
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    const int32_t channel = json_i(buf, "channel", -1);
    if (channel >= 0) return ok_json(req, c6_hub_reset_channel((uint8_t)channel));
    c6_hub_reset_race();
    return ok_json(req, true);
}

static esp_err_t post_volume(httpd_req_t *req) {
    char buf[128];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    c6_hub_set_volume((uint8_t)json_i(buf, "volume", 60));
    return ok_json(req, true);
}

/* The fleet's relative re-arm drop, {"rearm_db_x10": 0..400} (the page's longest body is 20 B):
 * saved and sent to every gate, or {"ok":false} out of range or absent. */
static esp_err_t post_rearm(httpd_req_t *req) {
    char buf[64];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    return ok_json(req, c6_hub_set_rearm(json_i(buf, "rearm_db_x10", -1)));
}

/* A JSON array of the gates set in mask. */
static int put_gates(char *b, size_t cap, uint32_t mask) {
    int o = snprintf(b, cap, "[");
    for (unsigned g = 0; g < RT_MAX_GATES; g++)
        if (mask >> g & 1u)
            o += snprintf(b + o, cap - o, "%s%u", (mask & ((1u << g) - 1u)) ? "," : "", g);
    return o + snprintf(b + o, cap - o, "]");
}

static const struct { const char *name; lp_cmd_t cmd; } k_cmds[] = {
    { "cal_full", LP_CMD_CAL_FULL }, { "cal_quick", LP_CMD_CAL_QUICK }, { "reboot", LP_CMD_REBOOT },
};

/* The C5 console's B / F / reset for gates nobody plugs USB into.
 * {"gate":0..15, -1 = every reachable gate, "cmd":"cal_full"|"cal_quick"|"reboot",
 * "force":0|1}. A calibration blinds the gate for its duration and wants every
 * VTX off: unless forced, a target already seeing a C/N above its active_db (a
 * VTX probably on) blocks the whole request and is listed in "vtx" for the admin
 * to confirm and force. Otherwise each target goes to "sent" or "failed", and
 * for a calibration a gate already calibrating (its C5 would refuse, on its USB
 * console only) is not sent but listed in "busy". */
static esp_err_t post_command(httpd_req_t *req) {
    char buf[128], name[16];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    const int32_t gate = json_i(buf, "gate", -2);
    lp_cmd_t cmd = 0;
    if (json_s(buf, "cmd", name, sizeof name))
        for (unsigned i = 0; i < sizeof k_cmds / sizeof k_cmds[0]; i++)
            if (strcmp(name, k_cmds[i].name) == 0) cmd = k_cmds[i].cmd;
    if (!cmd || gate < -1 || gate >= (int32_t)RT_MAX_GATES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cmd/gate"); return ESP_FAIL;
    }

    /* Targets as a gate mask (a gate shared by two nodes is one target), the
     * strongest C/N each one sees now and the gates already calibrating; an
     * offline node's or an absent local C5's last C/N and flags are stale (an
     * absent gate 0 asked by name lands in "failed"). */
    c6_store_snapshot(&s_snap);
    c6_hub_snapshot(&s_fleet);
    int32_t cn[RT_MAX_GATES];
    for (unsigned g = 0; g < RT_MAX_GATES; g++) cn[g] = LP_CN_UNKNOWN;
    uint32_t busy = 0;
    if (s_snap.has_local) {
        cn[0] = s_snap.local.cn_x10;
        if (s_snap.local.flags & LP_HEALTH_CALIBRATING) busy |= 1u;
    }
    uint32_t targets = (gate == 0 || (gate == -1 && s_snap.has_local)) ? 1u : 0u;
    bool known = gate <= 0;                    /* gate 0 is always the master's own */
    for (unsigned i = 0; i < s_fleet.num_nodes; i++) {
        const c6_node_t *n = &s_fleet.nodes[i];
        if (n->gate_index >= RT_MAX_GATES) continue;
        if (n->gate_index == gate) { known = true; targets |= 1u << gate; }
        if (!n->online) continue;
        if (gate == -1) targets |= 1u << n->gate_index;
        if (n->cn_x10 > cn[n->gate_index]) cn[n->gate_index] = n->cn_x10;
        if (n->flags & LP_HEALTH_CALIBRATING) busy |= 1u << n->gate_index;
    }
    if (!known) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gate"); return ESP_FAIL; }
    busy = cmd == LP_CMD_REBOOT ? 0u : busy & targets;   /* a reboot still reaches a busy C5 */
    targets &= ~busy;

    char out[640];   /* worst case: 16 vtx entries x 34 B */
    int o;
    uint32_t vtx = 0;
    /* Gate 0's own C5 tells its active_db; remote gates run the one the hub pushes. */
    const int32_t vtx_x10 = c6_hub_vtx_threshold_x10();
    const int32_t vtx0_x10 = s_snap.has_local_cfg ? s_snap.local_cfg.active_db_x10 : vtx_x10;
    if (cmd != LP_CMD_REBOOT && json_i(buf, "force", 0) == 0)
        for (unsigned g = 0; g < RT_MAX_GATES; g++)
            if ((targets >> g & 1u) && cn[g] != LP_CN_UNKNOWN && cn[g] > (g ? vtx_x10 : vtx0_x10))
                vtx |= 1u << g;
    if (vtx) {
        o = snprintf(out, sizeof out, "{\"ok\":false,\"vtx\":[");
        for (unsigned g = 0; g < RT_MAX_GATES; g++)
            if (vtx >> g & 1u)
                o += snprintf(out + o, sizeof out - o, "%s{\"gate\":%u,\"cn_x10\":%ld}",
                              (vtx & ((1u << g) - 1u)) ? "," : "", g, (long)cn[g]);
        o += snprintf(out + o, sizeof out - o, "]}");
        ESP_LOGW(TAG, "command %s gate %ld held: strong signal on gates 0x%04lx", name, (long)gate,
                 (unsigned long)vtx);
    } else {
        uint32_t sent = 0, failed = 0;
        for (unsigned g = 0; g < RT_MAX_GATES; g++)
            if (targets >> g & 1u) {
                if (c6_hub_send_command((uint8_t)g, cmd)) sent |= 1u << g;
                else failed |= 1u << g;
            }
        o = snprintf(out, sizeof out, "{\"ok\":true,\"sent\":");
        o += put_gates(out + o, sizeof out - o, sent);
        o += snprintf(out + o, sizeof out - o, ",\"failed\":");
        o += put_gates(out + o, sizeof out - o, failed);
        o += snprintf(out + o, sizeof out - o, ",\"busy\":");
        o += put_gates(out + o, sizeof out - o, busy);
        o += snprintf(out + o, sizeof out - o, "}");
        ESP_LOGI(TAG, "command %s gate %ld: sent 0x%04lx failed 0x%04lx busy 0x%04lx", name,
                 (long)gate, (unsigned long)sent, (unsigned long)failed, (unsigned long)busy);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, o);
}

/* Force a gate's C5 RX gain: {"gate":0..15,"gain":LP_GAIN_MIN..LP_GAIN_MAX,"force":0|1}. Changing it
 * makes the C5 learn its rests again (the gate is blind a moment), so it wants every VTX off: it
 * follows post_command's rule, answering {"ok":false,"vtx":[{"gate","cn_x10"}]} while the gate sees
 * a carrier, until the page asks the admin and retries with force. Otherwise {"ok":true,"sent":[g],
 * "failed":[],"busy":[]}: "failed" a gate nobody answers for (offline, no C5), "busy" a gate calibrating
 * (its C5 would take the gain, but the calibration's rests would be wrong). The new gain comes back
 * in /api/state. */
static esp_err_t post_gain(httpd_req_t *req) {
    char buf[64];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    const int32_t gate = json_i(buf, "gate", -1), gain = json_i(buf, "gain", -1);
    if (gate < 0 || gate >= (int32_t)RT_MAX_GATES || gain < (int32_t)LP_GAIN_MIN || gain > (int32_t)LP_GAIN_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gate/gain");
        return ESP_FAIL;
    }
    c6_store_snapshot(&s_snap);
    c6_hub_snapshot(&s_fleet);
    int32_t cn = LP_CN_UNKNOWN, active = c6_hub_vtx_threshold_x10();
    bool busy = false, known = gate == 0;
    if (gate == 0 && s_snap.has_local) {
        cn = s_snap.local.cn_x10;
        busy = s_snap.local.flags & LP_HEALTH_CALIBRATING;
        if (s_snap.has_local_cfg) active = s_snap.local_cfg.active_db_x10;
    }
    for (unsigned i = 0; i < s_fleet.num_nodes; i++) {
        const c6_node_t *n = &s_fleet.nodes[i];
        if (n->gate_index != gate) continue;
        known = true;
        if (!n->online) continue;
        if (n->cn_x10 > cn) cn = n->cn_x10;
        if (n->flags & LP_HEALTH_CALIBRATING) busy = true;
    }
    if (!known) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gate"); return ESP_FAIL; }

    char out[96];
    int o;
    if (json_i(buf, "force", 0) == 0 && cn != LP_CN_UNKNOWN && cn > active) {
        o = snprintf(out, sizeof out, "{\"ok\":false,\"vtx\":[{\"gate\":%ld,\"cn_x10\":%ld}]}", (long)gate, (long)cn);
        ESP_LOGW(TAG, "gain gate %ld held: strong signal", (long)gate);
    } else {
        const bool sent = !busy && c6_hub_send_gain((uint8_t)gate, (uint8_t)gain);
        const uint32_t bit = 1u << gate;
        o = snprintf(out, sizeof out, "{\"ok\":true,\"sent\":");
        o += put_gates(out + o, sizeof out - o, sent ? bit : 0u);
        o += snprintf(out + o, sizeof out - o, ",\"failed\":");
        o += put_gates(out + o, sizeof out - o, !busy && !sent ? bit : 0u);
        o += snprintf(out + o, sizeof out - o, ",\"busy\":");
        o += put_gates(out + o, sizeof out - o, busy ? bit : 0u);
        o += snprintf(out + o, sizeof out - o, "}");
        ESP_LOGI(TAG, "gain %ld gate %ld: %s", (long)gain, (long)gate, sent ? "sent" : busy ? "busy" : "failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, o);
}

/* Passage calibration (admin): with a drone in the passage, then 2 m away, a gate's C5 measures its
 * channels' smoothed rise and proposes thresholds (link_proto CALIB_*, docs/detector.md). It never
 * blinds the gate and the VTX stays ON, so there is no VTX guard; a pass rung during step 1 is real
 * (sound, a lap): the page says to reset that channel. POST {"gate":0..15,"action":"state" | "start1" |
 * "start2" | "cancel" | "apply" | "reset" (a remote gate back on the fleet's thresholds), "channel"?: 0..7 for start1, the
 * channel to measure, else any: the one standing highest} answers the session as {"ok","msg"?,"gate","phase" (0 none, 1 measuring
 * in the passage, 2 measured, 3 measuring away, 4 done),"secs_left","flags" (pass_calib.h PC_*),
 * "channel" (255 before step 1),"passage":[p10,p50,p90,p95,max] and "away":[...] (half dB),"passage_sat"
 * (% of clipped captures),"rec":{"active","hysteresis","drop","min_peak"} (dB x10, phase 4),
 * "gain_steps"}. "apply" sends the proposal to the gate's C5 and ends the session. */
static int put_calib(char *b, size_t cap, int32_t gate, bool ok, const char *msg, const lp_calib_t *k) {
    int o = snprintf(b, cap, "{\"ok\":%s,\"msg\":\"%s\",\"gate\":%ld,\"phase\":%u,\"secs_left\":%u,\"flags\":%u,"
                     "\"channel\":%u,\"passage\":[%d,%d,%d,%d,%d],\"passage_sat\":%u,\"away\":[%d,%d,%d,%d,%d],"
                     "\"rec\":{\"active\":%d,\"hysteresis\":%d,\"drop\":%d,\"min_peak\":%d},\"gain_steps\":%d}",
                     ok ? "true" : "false", msg, (long)gate, k->phase, k->secs_left, k->flags, k->channel,
                     k->gate[0], k->gate[1], k->gate[2], k->gate[3], k->gate[4], k->gate_sat_pct,
                     k->away[0], k->away[1], k->away[2], k->away[3], k->away[4],
                     k->active_db_x10, k->hysteresis_db_x10, k->drop_db_x10, k->min_peak_db_x10, k->gain_steps);
    return o;
}

static esp_err_t post_calib(httpd_req_t *req) {
    char buf[64], action[12];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    const int32_t gate = json_i(buf, "gate", -1);
    if (gate < 0 || gate >= (int32_t)RT_MAX_GATES || !json_s(buf, "action", action, sizeof action)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gate/action");
        return ESP_FAIL;
    }
    bool ok = true;
    const char *msg = "";
    const int32_t chv = json_i(buf, "channel", LP_CALIB_ANY);      /* step 1's: R1..R8 as 0..7, else any */
    const uint8_t ch = chv >= 0 && chv < (int32_t)LP_LEVELS ? (uint8_t)chv : LP_CALIB_ANY;
    if (strcmp(action, "start1") == 0) ok = c6_hub_calib_start((uint8_t)gate, 1, ch);
    else if (strcmp(action, "start2") == 0) ok = c6_hub_calib_start((uint8_t)gate, 2, LP_CALIB_ANY);
    else if (strcmp(action, "cancel") == 0) { c6_hub_calib_start((uint8_t)gate, 0, LP_CALIB_ANY); }
    else if (strcmp(action, "apply") == 0) { ok = c6_hub_calib_apply((uint8_t)gate); if (!ok) msg = "nothing to apply"; }
    else if (strcmp(action, "reset") == 0) { ok = c6_hub_calib_reset((uint8_t)gate); if (!ok) msg = gate == 0 ? "gate 0 keeps its own settings" : "no such gate"; }
    else if (strcmp(action, "state") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action");
        return ESP_FAIL;
    }
    if (!ok && !msg[0]) msg = "no C5 linked";
    lp_calib_t k;
    (void)c6_hub_calib_get((uint8_t)gate, &k);
    static char out[480];                           /* larger than the stack; httpd runs one handler at a time */
    const int o = put_calib(out, sizeof out, gate, ok, msg, &k);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, o);
}

/* Debug mode (admin): a gate's C5 console text and typed keys. Every body may carry "gate" (default
 * 0, the master's own C5 over its UART; 1.. a remote gate, over its node: c6_hub_debug_*). One gate
 * at a time is in debug mode: switching one on switches the previous off. */
static int32_t body_gate(const char *buf) {
    const int32_t g = json_i(buf, "gate", 0);
    return g >= 0 && g < (int32_t)RT_MAX_GATES ? g : -1;
}

/* Is gate g in debug mode now? */
static bool dbg_on(int32_t g) { return g == 0 ? c6_link_debug() : c6_hub_debug_gate() == g; }

/* Is gate g's C5 reachable (the local one heard, a node of that gate online)? s_fleet is refreshed. */
static bool dbg_reachable(int32_t g) {
    if (g == 0) return c6_store_has_local();
    c6_hub_snapshot(&s_fleet);
    for (unsigned i = 0; i < s_fleet.num_nodes; i++)
        if (s_fleet.nodes[i].gate_index == g && s_fleet.nodes[i].online) return true;
    return false;
}

/* POST {"since":N,"gate":G} answers {"gate","debug","next","lost","c5_alive","text"}: the bytes after
 * position N of the debug log (the text holds no quote, backslash or control other than the
 * newline, which is escaped here). The page asks again from "next"; "gate" tells it which gate the
 * answer is for (it may have changed the selector meanwhile). A gate that is not the one in debug
 * mode answers debug 0 and no text; a remote gate's poll keeps its debug mode alive. */
static esp_err_t post_log(httpd_req_t *req) {
    char buf[64];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    static char raw[C6_LOG_CHUNK];                  /* larger than the stack; httpd runs one handler at a time */
    static char body[2 * C6_LOG_CHUNK + 96];
    const int32_t gate = body_gate(buf);
    const bool on = gate >= 0 && dbg_on(gate);
    uint32_t next = 0;
    bool lost = false;
    size_t n = 0;
    if (on) {
        const uint32_t since = (uint32_t)json_i(buf, "since", 0);
        n = gate == 0 ? c6_link_log_read(since, raw, sizeof raw, &next, &lost)
                      : c6_hub_log_read(since, raw, sizeof raw, &next, &lost);
    }
    int o = snprintf(body, sizeof body, "{\"gate\":%ld,\"debug\":%u,\"next\":%lu,\"lost\":%u,\"c5_alive\":%u,\"text\":\"",
                     (long)gate, on ? 1u : 0u, (unsigned long)next, lost ? 1u : 0u,
                     gate >= 0 && dbg_reachable(gate) ? 1u : 0u);
    for (size_t i = 0; i < n; i++) {
        if (raw[i] == '\n') { body[o++] = '\\'; body[o++] = 'n'; }
        else body[o++] = raw[i];
    }
    o += snprintf(body + o, sizeof body - o, "\"}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, o);
}

/* Debug mode of a gate on or off ({"on":0|1,"gate":G}; no "on" only asks). Off at every boot: nothing
 * is mirrored from any C5, and /api/key sends nothing, until an admin switches a gate on. A remote
 * gate with no online node answers {"ok":false}. */
static esp_err_t post_debug(httpd_req_t *req) {
    char buf[32];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    const int32_t on = json_i(buf, "on", -1), gate = body_gate(buf);
    if (gate < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gate"); return ESP_FAIL; }
    bool ok = true;
    if (on > 0) {
        const int remote = c6_hub_debug_gate();
        if (gate == 0) {
            if (remote > 0) c6_hub_debug_set((uint8_t)remote, false);   /* one gate at a time */
            c6_link_debug_set(true);
        } else {
            if (c6_link_debug()) c6_link_debug_set(false);
            ok = c6_hub_debug_set((uint8_t)gate, true);
        }
    } else if (on == 0) {
        if (gate == 0) c6_link_debug_set(false);
        else c6_hub_debug_set((uint8_t)gate, false);
    }
    if (on >= 0) ESP_LOGW(TAG, "debug mode gate %ld %s from web%s", (long)gate, on ? "ON" : "off", ok ? "" : " (refused)");
    char r[48];
    snprintf(r, sizeof r, "{\"ok\":%s,\"debug\":%u}", ok ? "true" : "false", dbg_on(gate) ? 1u : 0u);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, r);
}

/* {"keys":"Ka","force":0|1,"gate":G}: typed on the gate's C5 console as on its USB ("K" opens the
 * settings menu, "T" is the status, "F"/"B" calibrate). The keys that blind the gate follow
 * post_command's rule: refused with {"blind":true}, or {"vtx":true} while a carrier is seen
 * (gate 0's C/N against its active_db, a remote gate's against the one the hub pushes), until the
 * page asks the admin and retries with force. The C5's answer comes back through /api/log. */
static esp_err_t post_key(httpd_req_t *req) {
    char buf[96], keys[LP_KEY_MAX + 1];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    const int32_t gate = body_gate(buf);
    if (gate < 0 || !json_s(buf, "keys", keys, sizeof keys) || !keys[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no keys");
        return ESP_FAIL;
    }
    if (!lp_keys_valid(keys)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad key");
        return ESP_FAIL;
    }
    const bool force = json_i(buf, "force", 0) != 0;

    httpd_resp_set_type(req, "application/json");
    if (!dbg_on(gate))
        return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"debug mode is off\"}");
    if (!dbg_reachable(gate))
        return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"no C5 linked\"}");
    if (lp_keys_blind(keys) && !force) {
        c6_store_snapshot(&s_snap);
        c6_hub_snapshot(&s_fleet);
        int32_t cn = LP_CN_UNKNOWN, active = c6_hub_vtx_threshold_x10();
        if (gate == 0) {
            cn = s_snap.local.cn_x10;
            if (s_snap.has_local_cfg) active = s_snap.local_cfg.active_db_x10;
        } else {
            for (unsigned i = 0; i < s_fleet.num_nodes; i++)
                if (s_fleet.nodes[i].gate_index == gate && s_fleet.nodes[i].online && s_fleet.nodes[i].cn_x10 > cn)
                    cn = s_fleet.nodes[i].cn_x10;
        }
        char r[72];
        if (cn != LP_CN_UNKNOWN && cn > active)
            snprintf(r, sizeof r, "{\"ok\":false,\"vtx\":true,\"cn_x10\":%ld}", (long)cn);
        else
            snprintf(r, sizeof r, "{\"ok\":false,\"blind\":true}");
        return httpd_resp_sendstr(req, r);
    }
    if (gate == 0) c6_link_send_keys(keys, strlen(keys));
    else if (!c6_hub_send_keys((uint8_t)gate, keys, strlen(keys)))
        return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"not sent\"}");
    ESP_LOGW(TAG, "keys \"%s\" to gate %ld from web%s", keys, (long)gate, force ? " (forced)" : "");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void c6_web_start(void) {
    web_auth_init(s_admin_key, ADMIN_SALT, ADMIN_ITERATIONS, ADMIN_AUTH_OPEN);
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 18;   /* 16 here + web_auth's 2 */
    cfg.stack_size = 6144;       /* the signature's PSA HMAC under post_command: ~3.5 KB of the 4 KB default */
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = get_root },
        { .uri = "/api/state",    .method = HTTP_GET,  .handler = get_state },
        { .uri = "/api/results",  .method = HTTP_GET,  .handler = get_results },
        { .uri = "/api/auth",     .method = HTTP_GET,  .handler = get_auth },
        { .uri = "/api/pairing",  .method = HTTP_POST, .handler = post_pairing },
        { .uri = "/api/node",     .method = HTTP_POST, .handler = post_node },
        { .uri = "/api/node_remove", .method = HTTP_POST, .handler = post_node_remove },
        { .uri = "/api/reset",    .method = HTTP_POST, .handler = post_reset },
        { .uri = "/api/volume",   .method = HTTP_POST, .handler = post_volume },
        { .uri = "/api/rearm",    .method = HTTP_POST, .handler = post_rearm },
        { .uri = "/api/command",  .method = HTTP_POST, .handler = post_command },
        { .uri = "/api/gain",     .method = HTTP_POST, .handler = post_gain },
        { .uri = "/api/calib",    .method = HTTP_POST, .handler = post_calib },
        { .uri = "/api/log",      .method = HTTP_POST, .handler = post_log },
        { .uri = "/api/key",      .method = HTTP_POST, .handler = post_key },
        { .uri = "/api/debug",    .method = HTTP_POST, .handler = post_debug },
    };
    for (unsigned i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(srv, &uris[i]);
    web_auth_register(srv);   /* POST /api/session, GET /auth.js */
    ESP_LOGI(TAG, "admin web on http://192.168.4.1/");
}
