#include "c3_web.h"
#include "c3_link.h"
#include "c3_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "c3_web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t get_state(httpd_req_t *req)
{
    c3_snapshot_t s;
    c3_store_snapshot(&s);

    char body[2048];
    int o = 0;
    o += snprintf(body + o, sizeof body - o,
                  "{\"has_config\":%s,\"config\":{\"alpha_milli\":%ld,\"active_db_x10\":%ld,"
                  "\"hysteresis_db_x10\":%ld,\"drop_db_x10\":%ld,\"min_peak_db_x10\":%ld,"
                  "\"refractory_ms\":%lu,\"stale_ms\":%lu,\"volume_pct\":%u,\"muted\":%u},",
                  s.has_config ? "true" : "false",
                  (long)s.config.alpha_milli, (long)s.config.active_db_x10,
                  (long)s.config.hysteresis_db_x10, (long)s.config.drop_db_x10,
                  (long)s.config.min_peak_db_x10, (unsigned long)s.config.refractory_ms,
                  (unsigned long)s.config.stale_ms, s.config.volume_pct, s.config.muted);
    o += snprintf(body + o, sizeof body - o,
                  "\"has_stats\":%s,\"stats\":{\"played\":%lu,\"dropped\":%lu,\"muted\":%lu,"
                  "\"failed\":%lu,\"cn_x10\":%ld,\"channel\":%u,\"state\":%u},\"total_events\":%lu,"
                  "\"events\":[",
                  s.has_stats ? "true" : "false", (unsigned long)s.stats.played,
                  (unsigned long)s.stats.dropped, (unsigned long)s.stats.muted,
                  (unsigned long)s.stats.failed, (long)s.stats.cn_x10, s.stats.channel,
                  s.stats.state, (unsigned long)s.total_events);
    for (unsigned i = 0; i < s.n_events && o < (int)sizeof body; i++)
        o += snprintf(body + o, sizeof body - o,
                      "%s{\"channel\":%u,\"peak_db_x10\":%ld,\"at_ms\":%lu}",
                      i ? "," : "", s.events[i].channel, (long)s.events[i].peak_db_x10,
                      (unsigned long)s.events[i].at_ms);
    o += snprintf(body + o, sizeof body - o, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, o);
}

/* Tiny parser for the flat integer object our own web client POSTs: find
 * "key", skip to the colon, read the number. No general JSON needed. */
static int32_t json_i(const char *body, const char *key, int32_t def)
{
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

static esp_err_t post_config(httpd_req_t *req)
{
    char buf[512];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof buf) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        got += r;
    }
    buf[got] = '\0';

    lp_config_t cur;
    (void)c3_store_get_config(&cur);           /* start from current, override given keys */
    cur.alpha_milli       = json_i(buf, "alpha_milli", cur.alpha_milli);
    cur.active_db_x10     = json_i(buf, "active_db_x10", cur.active_db_x10);
    cur.hysteresis_db_x10 = json_i(buf, "hysteresis_db_x10", cur.hysteresis_db_x10);
    cur.drop_db_x10       = json_i(buf, "drop_db_x10", cur.drop_db_x10);
    cur.min_peak_db_x10   = json_i(buf, "min_peak_db_x10", cur.min_peak_db_x10);
    cur.refractory_ms     = (uint32_t)json_i(buf, "refractory_ms", (int32_t)cur.refractory_ms);
    cur.stale_ms          = (uint32_t)json_i(buf, "stale_ms", (int32_t)cur.stale_ms);
    cur.volume_pct        = (uint8_t)json_i(buf, "volume_pct", cur.volume_pct);
    cur.muted             = (uint8_t)json_i(buf, "muted", cur.muted);

    if (cur.volume_pct > 100) cur.volume_pct = 100;
    cur.muted = cur.muted ? 1 : 0;

    c3_store_set_config(&cur, true);
    c3_link_push_config(&cur);
    ESP_LOGI(TAG, "config updated from web");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void c3_web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }

    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = get_root };
    const httpd_uri_t state = { .uri = "/api/state", .method = HTTP_GET, .handler = get_state };
    const httpd_uri_t conf = { .uri = "/api/config", .method = HTTP_POST, .handler = post_config };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &state);
    httpd_register_uri_handler(srv, &conf);
    ESP_LOGI(TAG, "web UI on http://192.168.4.1/");
}
