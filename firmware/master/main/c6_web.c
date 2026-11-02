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

static esp_err_t get_state(httpd_req_t *req) {
    c6_snapshot_t s;
    c6_store_snapshot(&s);
    c6_fleet_t f;
    c6_hub_snapshot(&f);

    static char body[3072];   /* larger than the stack; this handler is serialised by httpd */
    int o = 0;
    o += snprintf(body + o, sizeof body - o,
                  "{\"pairing\":%s,\"num_gates\":%u,\"has_local\":%s,"
                  "\"local\":{\"channel\":%u,\"cn_x10\":%ld,\"state\":%u,\"played\":%lu,"
                  "\"dropped\":%lu,\"failed\":%lu},\"total_passes\":%lu,\"volume\":%u,"
                  "\"uptime_ms\":%lu,\"free_heap\":%lu,",
                  f.pairing ? "true" : "false", f.num_gates, s.has_local ? "true" : "false",
                  s.local.channel, (long)s.local.cn_x10, s.local.state, (unsigned long)s.local.played,
                  (unsigned long)s.local.dropped, (unsigned long)s.local.failed,
                  (unsigned long)s.total_passes, (unsigned)f.volume,
                  (unsigned long)f.uptime_ms, (unsigned long)f.free_heap);

    o += snprintf(body + o, sizeof body - o, "\"nodes\":[");
    for (unsigned i = 0; i < f.num_nodes && o < (int)sizeof body; i++) {
        const c6_node_t *n = &f.nodes[i];
        o += snprintf(body + o, sizeof body - o,
                      "%s{\"node_id\":%u,\"gate\":%u,\"channel\":%u,\"online\":%s,\"cn_x10\":%ld,"
                      "\"state\":%u,\"last_seen_ms\":%lu,"
                      "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
                      i ? "," : "", n->node_id, n->gate_index, n->channel,
                      n->online ? "true" : "false", (long)n->cn_x10,
                      n->state, (unsigned long)n->last_seen_ms,
                      n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
    }
    o += snprintf(body + o, sizeof body - o, "],\"standings\":[");
    for (unsigned i = 0; i < f.num_standings && o < (int)sizeof body; i++) {
        const c6_standing_t *st = &f.standings[i];
        o += snprintf(body + o, sizeof body - o,
                      "%s{\"channel\":%u,\"laps\":%lu,\"last_lap_ms\":%lu,\"best_lap_ms\":%lu,\"last_gate\":%d}",
                      i ? "," : "", st->channel, (unsigned long)st->laps,
                      (unsigned long)st->last_lap_ms, (unsigned long)st->best_lap_ms, st->last_gate);
    }
    o += snprintf(body + o, sizeof body - o, "],\"passes\":[");
    for (unsigned i = 0; i < s.n_passes && o < (int)sizeof body; i++)
        o += snprintf(body + o, sizeof body - o,
                      "%s{\"gate\":%u,\"channel\":%u,\"peak_db_x10\":%ld,\"at_ms\":%lu}",
                      i ? "," : "", s.passes[i].gate, s.passes[i].channel,
                      (long)s.passes[i].peak_db_x10, (unsigned long)s.passes[i].at_ms);
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

/* ---- admin auth (spectators are read-only; admin actions need a login) ---- */
static char s_auth[160];   /* expected "Basic <base64(user:pass)>", empty = open */

static void build_auth(void) {
    if (CONFIG_C6_ADMIN_PASSWORD[0] == '\0') { s_auth[0] = '\0'; return; }  /* auth disabled */
    char up[96];
    int n = snprintf(up, sizeof up, "%s:%s", CONFIG_C6_ADMIN_USER, CONFIG_C6_ADMIN_PASSWORD);
    unsigned char b64[128];
    size_t olen = 0;
    if (n <= 0 || mbedtls_base64_encode(b64, sizeof b64, &olen, (const unsigned char *)up,
                                        (size_t)n) != 0) { s_auth[0] = '\0'; return; }
    snprintf(s_auth, sizeof s_auth, "Basic %.*s", (int)olen, (const char *)b64);
}

static bool require_admin(httpd_req_t *req) {
    if (s_auth[0] == '\0') return true;   /* admin auth disabled */
    char hdr[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK &&
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
    };
    for (unsigned i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(srv, &uris[i]);
    ESP_LOGI(TAG, "admin web on http://192.168.4.1/");
}
