#include "c6_web.h"
#include "c6_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <stdio.h>

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

    char body[2048];
    int o = 0;
    o += snprintf(body + o, sizeof body - o,
                  "{\"has_local\":%s,\"local\":{\"channel\":%u,\"cn_x10\":%ld,\"state\":%u,"
                  "\"played\":%lu,\"dropped\":%lu,\"failed\":%lu},\"total_passes\":%lu,\"passes\":[",
                  s.has_local ? "true" : "false", s.local.channel, (long)s.local.cn_x10,
                  s.local.state, (unsigned long)s.local.played, (unsigned long)s.local.dropped,
                  (unsigned long)s.local.failed, (unsigned long)s.total_passes);
    for (unsigned i = 0; i < s.n_passes && o < (int)sizeof body; i++)
        o += snprintf(body + o, sizeof body - o,
                      "%s{\"gate\":%u,\"channel\":%u,\"peak_db_x10\":%ld,\"at_ms\":%lu}",
                      i ? "," : "", s.passes[i].gate, s.passes[i].channel,
                      (long)s.passes[i].peak_db_x10, (unsigned long)s.passes[i].at_ms);
    o += snprintf(body + o, sizeof body - o, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, o);
}

void c6_web_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = get_root };
    const httpd_uri_t state = { .uri = "/api/state", .method = HTTP_GET, .handler = get_state };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &state);
    ESP_LOGI(TAG, "admin web on http://192.168.4.1/");
}
