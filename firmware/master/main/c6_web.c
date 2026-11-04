#include "c6_web.h"
#include "c6_hub.h"
#include "c6_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
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

/* A C5's firmware id is foreign text: keep it inert in the JSON and in the
 * page's HTML (printable ASCII minus quotes, backslash and markup). */
static const char *safe_fw(const char *fw, char out[LP_FW_LEN]) {
    unsigned i = 0;
    for (; i + 1u < LP_FW_LEN && fw[i]; i++)
        out[i] = (fw[i] >= 0x20 && fw[i] < 0x7f && !strchr("\"\\<>&'", fw[i])) ? fw[i] : '?';
    out[i] = '\0';
    return out;
}

/* The C5 health fields shared by "local" and each node. */
static int put_health(char *b, size_t cap, uint32_t passes, uint8_t flags, uint8_t gain,
                      uint32_t uptime_s, uint32_t heap_min, uint32_t faults, const char *fw) {
    char f[LP_FW_LEN];
    return snprintf(b, cap, "\"passes\":%lu,\"cal_needed\":%s,\"calibrating\":%s,\"gain\":%u,"
                    "\"c5_uptime_s\":%lu,\"heap_min\":%lu,\"faults\":%lu,\"fw\":\"%s\"",
                    (unsigned long)passes, (flags & LP_HEALTH_CAL_NEEDED) ? "true" : "false",
                    (flags & LP_HEALTH_CALIBRATING) ? "true" : "false", gain,
                    (unsigned long)uptime_s, (unsigned long)heap_min, (unsigned long)faults,
                    safe_fw(fw, f));
}

static esp_err_t get_state(httpd_req_t *req) {
    c6_store_snapshot(&s_snap);
    c6_hub_snapshot(&s_fleet);
    const c6_snapshot_t *s = &s_snap;
    const c6_fleet_t *f = &s_fleet;

    /* Worst case (every field at its widest) 8852 B: header + local 394,
     * 15 nodes x 392, 8 standings 838, 24 passes 1739; sized so it never truncates. */
    static char body[10240];   /* larger than the stack; this handler is serialised by httpd */
    const lp_stats_t *l = &s->local;
    int o = 0;
    o += snprintf(body + o, sizeof body - o,
                  "{\"pairing\":%s,\"num_gates\":%u,\"has_local\":%s,"
                  "\"local\":{\"channel\":%u,\"cn_x10\":%ld,\"state\":%u,",
                  f->pairing ? "true" : "false", f->num_gates, s->has_local ? "true" : "false",
                  l->channel, (long)l->cn_x10, l->state);
    o += put_health(body + o, sizeof body - o, l->passes, l->flags, l->gain, l->uptime_s,
                    l->heap_min, l->faults, l->fw);
    o += snprintf(body + o, sizeof body - o,
                  "},\"total_passes\":%lu,\"volume\":%u,"
                  "\"uptime_ms\":%lu,\"free_heap\":%lu,\"heap_total\":%lu,",
                  (unsigned long)s->total_passes, (unsigned)f->volume,
                  (unsigned long)f->uptime_ms, (unsigned long)f->free_heap,
                  (unsigned long)f->heap_total);

    o += snprintf(body + o, sizeof body - o, "\"nodes\":[");
    for (unsigned i = 0; i < f->num_nodes && o < (int)sizeof body; i++) {
        const c6_node_t *n = &f->nodes[i];
        o += snprintf(body + o, sizeof body - o,
                      "%s{\"node_id\":%u,\"gate\":%u,\"channel\":%u,\"online\":%s,\"cn_x10\":%ld,"
                      "\"state\":%u,\"last_seen_ms\":%lu,\"rssi\":%d,\"rx_frames\":%lu,"
                      "\"status_rx\":%lu,\"status_lost\":%lu,"
                      "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",",
                      i ? "," : "", n->node_id, n->gate_index, n->channel,
                      n->online ? "true" : "false", (long)n->cn_x10,
                      n->state, (unsigned long)n->last_seen_ms,
                      (int)n->rssi, (unsigned long)n->rx_frames,
                      (unsigned long)n->status_rx, (unsigned long)n->status_lost,
                      n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
        o += put_health(body + o, sizeof body - o, n->passes, n->flags, n->gain, n->c5_uptime_s,
                        n->heap_min, n->faults, n->fw);
        o += snprintf(body + o, sizeof body - o, "}");
    }
    o += snprintf(body + o, sizeof body - o, "],\"standings\":[");
    for (unsigned i = 0; i < f->num_standings && o < (int)sizeof body; i++) {
        const c6_standing_t *st = &f->standings[i];
        o += snprintf(body + o, sizeof body - o,
                      "%s{\"channel\":%u,\"laps\":%lu,\"last_lap_ms\":%lu,\"best_lap_ms\":%lu,\"last_gate\":%d}",
                      i ? "," : "", st->channel, (unsigned long)st->laps,
                      (unsigned long)st->last_lap_ms, (unsigned long)st->best_lap_ms, st->last_gate);
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

static bool read_body(httpd_req_t *req, char *buf, size_t cap) {
    int total = req->content_len;
    if (total <= 0 || total >= (int)cap) return false;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return false;
        got += r;
    }
    buf[got] = '\0';
    return true;
}

static esp_err_t ok_json(httpd_req_t *req, bool ok) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* ---- admin auth (spectators are read-only; admin actions need the password) ---- */
/* 128 bytes of password make a 178-byte Authorization header, well inside the
 * 1 KiB request-header budget (CONFIG_HTTPD_MAX_REQ_HDR_LEN) next to a phone
 * browser's other headers. A longer one fails the build. */
#define ADMIN_PASSWORD_MAX 128
_Static_assert(sizeof CONFIG_C6_ADMIN_PASSWORD - 1 <= ADMIN_PASSWORD_MAX,
               "CONFIG_C6_ADMIN_PASSWORD is longer than 128 bytes");
#define B64_LEN(n) (4 * (((n) + 2) / 3))
/* expected "Basic <base64(:pass)>" (no username); empty = locked, never open */
static char s_auth[sizeof "Basic " + B64_LEN(1 + ADMIN_PASSWORD_MAX)];

static void build_auth(void) {
    if (CONFIG_C6_ADMIN_PASSWORD[0] == '\0') return;   /* auth disabled */
    char up[1 + ADMIN_PASSWORD_MAX + 1];
    const int n = snprintf(up, sizeof up, ":%s", CONFIG_C6_ADMIN_PASSWORD);
    size_t olen = 0;
    memcpy(s_auth, "Basic ", 6);
    if (n < 0 || (size_t)n >= sizeof up ||
        mbedtls_base64_encode((unsigned char *)s_auth + 6, sizeof s_auth - 6, &olen,
                              (const unsigned char *)up, (size_t)n) != 0) {
        s_auth[0] = '\0';
        ESP_LOGE(TAG, "admin password encoding failed: admin actions locked");
    }
}

static bool require_admin(httpd_req_t *req) {
    if (CONFIG_C6_ADMIN_PASSWORD[0] == '\0') return true;   /* admin auth disabled */
    /* One spare byte: ESP-IDF truncates a value exactly the buffer's size and still
     * returns ESP_OK, so a longer header always mismatches here. */
    char hdr[sizeof s_auth + 1];
    if (s_auth[0] != '\0' &&
        httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK &&
        strcmp(hdr, s_auth) == 0)
        return true;
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"auth\":true}"), false;
}

static esp_err_t get_auth(httpd_req_t *req) {
    if (!require_admin(req)) return ESP_OK;   /* 401 already sent */
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"admin\":true}");
}

static esp_err_t post_pairing(httpd_req_t *req) {
    if (!require_admin(req)) return ESP_OK;
    char buf[128];
    if (!read_body(req, buf, sizeof buf)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    c6_hub_set_pairing(json_i(buf, "open", 0) != 0);
    return ok_json(req, true);
}

static esp_err_t post_node(httpd_req_t *req) {
    if (!require_admin(req)) return ESP_OK;
    char buf[128];
    if (!read_body(req, buf, sizeof buf)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    bool ok = c6_hub_set_node((uint8_t)json_i(buf, "node_id", -1),
                              (uint8_t)json_i(buf, "gate", 0),
                              (uint8_t)json_i(buf, "channel", 0));
    return ok_json(req, ok);
}

static esp_err_t post_node_remove(httpd_req_t *req) {
    if (!require_admin(req)) return ESP_OK;
    char buf[128];
    if (!read_body(req, buf, sizeof buf)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    return ok_json(req, c6_hub_remove_node((uint8_t)json_i(buf, "node_id", -1)));
}

static esp_err_t post_reset(httpd_req_t *req) {
    if (!require_admin(req)) return ESP_OK;
    c6_hub_reset_race();
    return ok_json(req, true);
}

static esp_err_t post_volume(httpd_req_t *req) {
    if (!require_admin(req)) return ESP_OK;
    char buf[128];
    if (!read_body(req, buf, sizeof buf)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    c6_hub_set_volume((uint8_t)json_i(buf, "volume", 60));
    return ok_json(req, true);
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
    if (!require_admin(req)) return ESP_OK;
    char buf[128], name[16];
    if (!read_body(req, buf, sizeof buf)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
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

void c6_web_start(void) {
    build_auth();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 10;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = get_root },
        { .uri = "/api/state",    .method = HTTP_GET,  .handler = get_state },
        { .uri = "/api/auth",     .method = HTTP_GET,  .handler = get_auth },
        { .uri = "/api/pairing",  .method = HTTP_POST, .handler = post_pairing },
        { .uri = "/api/node",     .method = HTTP_POST, .handler = post_node },
        { .uri = "/api/node_remove", .method = HTTP_POST, .handler = post_node_remove },
        { .uri = "/api/reset",    .method = HTTP_POST, .handler = post_reset },
        { .uri = "/api/volume",   .method = HTTP_POST, .handler = post_volume },
        { .uri = "/api/command",  .method = HTTP_POST, .handler = post_command },
    };
    for (unsigned i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(srv, &uris[i]);
    ESP_LOGI(TAG, "admin web on http://192.168.4.1/");
}
