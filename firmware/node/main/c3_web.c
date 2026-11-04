#include "c3_web.h"
#include "c3_link.h"
#include "c3_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
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

/* The C5's firmware id comes off the wire: keep it a valid JSON string by
 * passing only printable ASCII other than '"' and '\\'. */
static void json_safe(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    for (; src[i] && i + 1u < cap; i++) {
        const unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= ' ' && c <= '~' && c != '"' && c != '\\') ? (char)c : '?';
    }
    dst[i] = '\0';
}

static esp_err_t get_state(httpd_req_t *req)
{
    c3_snapshot_t s;
    c3_store_snapshot(&s);
    char fw[LP_FW_LEN];
    json_safe(fw, s.stats.fw, sizeof fw);

    char body[2048];   /* worst case (16 events, every field at its widest): 1534 B */
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
                  "\"c5_alive\":%s,"
                  "\"has_stats\":%s,\"stats\":{\"cn_x10\":%ld,\"channel\":%u,\"state\":%u,"
                  "\"flags\":%u,\"cal_needed\":%s,\"calibrating\":%s,\"gain\":%u,\"passes\":%lu,"
                  "\"uptime_s\":%lu,\"heap_min\":%lu,\"faults\":%lu,\"fw\":\"%s\"},"
                  "\"total_events\":%lu,\"events\":[",
                  c3_link_c5_alive() ? "true" : "false",
                  s.has_stats ? "true" : "false", (long)s.stats.cn_x10, s.stats.channel,
                  s.stats.state, s.stats.flags,
                  (s.stats.flags & LP_HEALTH_CAL_NEEDED) ? "true" : "false",
                  (s.stats.flags & LP_HEALTH_CALIBRATING) ? "true" : "false", s.stats.gain,
                  (unsigned long)s.stats.passes, (unsigned long)s.stats.uptime_s,
                  (unsigned long)s.stats.heap_min, (unsigned long)s.stats.faults, fw,
                  (unsigned long)s.total_events);
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

/* Same for a short string value: copy "key":"value" into out[cap]; false when
 * absent, not a string or too long. */
static bool json_s(const char *body, const char *key, char *out, size_t cap)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':'))) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '"') return false;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1u < cap) n++;
    if (p[n] != '"') return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* Read the whole POST body into buf[cap] as a C string; on failure reply 400. */
static bool recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    const int total = req->content_len;
    if (total <= 0 || total >= (int)cap) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return false;
    }
    int got = 0;
    while (got < total) {
        const int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return false; }
        got += r;
    }
    buf[got] = '\0';
    return true;
}

/* ---- admin auth (spectators read results; config and commands need the password) ---- */
static char s_auth[160];   /* expected "Basic <base64(:pass)>" (no username), empty = open */

static void build_auth(void)
{
    if (CONFIG_C3_ADMIN_PASSWORD[0] == '\0') { s_auth[0] = '\0'; return; }
    char up[96];
    int n = snprintf(up, sizeof up, ":%s", CONFIG_C3_ADMIN_PASSWORD);
    unsigned char b64[128];
    size_t olen = 0;
    if (n <= 0 || mbedtls_base64_encode(b64, sizeof b64, &olen, (const unsigned char *)up,
                                        (size_t)n) != 0) { s_auth[0] = '\0'; return; }
    snprintf(s_auth, sizeof s_auth, "Basic %.*s", (int)olen, (const char *)b64);
}

static bool require_admin(httpd_req_t *req)
{
    if (s_auth[0] == '\0') return true;
    char hdr[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK &&
        strcmp(hdr, s_auth) == 0)
        return true;
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"auth\":true}"), false;
}

static esp_err_t get_auth(httpd_req_t *req)
{
    if (!require_admin(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"admin\":true}");
}

static esp_err_t post_config(httpd_req_t *req)
{
    if (!require_admin(req)) return ESP_OK;
    char buf[512];
    if (!recv_body(req, buf, sizeof buf)) return ESP_FAIL;

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
    const int32_t ch = json_i(buf, "channel", -1);   /* optional band selection */
    if (ch >= 0) c3_link_set_channel((uint8_t)ch);
    ESP_LOGI(TAG, "config updated from web");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* What the console's B, F and a reset do, for a C5 nobody plugs USB into:
 * {"cmd":"cal_full"|"cal_quick"|"reboot","force":0|1}. A calibration blinds
 * the gate and wants the VTXs off, so unless forced it is refused while the
 * C5 sees a carrier above the activity threshold (a VTX is probably on); the
 * page then asks the admin and retries with force. Any command is refused,
 * forced or not, while no C5 is linked: nothing would run it. */
static esp_err_t post_command(httpd_req_t *req)
{
    if (!require_admin(req)) return ESP_OK;
    char buf[96], name[16];
    if (!recv_body(req, buf, sizeof buf)) return ESP_FAIL;
    if (!json_s(buf, "cmd", name, sizeof name)) name[0] = '\0';
    lp_cmd_t cmd;
    if (strcmp(name, "cal_full") == 0)       cmd = LP_CMD_CAL_FULL;
    else if (strcmp(name, "cal_quick") == 0) cmd = LP_CMD_CAL_QUICK;
    else if (strcmp(name, "reboot") == 0)    cmd = LP_CMD_REBOOT;
    else { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown cmd"); return ESP_FAIL; }
    const bool force = json_i(buf, "force", 0) != 0;

    httpd_resp_set_type(req, "application/json");
    if (!c3_link_c5_alive())   /* silent C5: its last C/N is stale, and no one would listen */
        return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"no C5 linked\"}");
    if (cmd != LP_CMD_REBOOT && !force) {
        c3_snapshot_t s;
        c3_store_snapshot(&s);   /* config: the C5's CONFIG_STATE, else the stored one or the default */
        if (s.has_stats && s.stats.cn_x10 != LP_CN_UNKNOWN &&
            s.stats.cn_x10 > s.config.active_db_x10) {
            char r[64];
            snprintf(r, sizeof r, "{\"ok\":false,\"vtx\":true,\"cn_x10\":%ld}", (long)s.stats.cn_x10);
            return httpd_resp_sendstr(req, r);
        }
    }
    c3_link_send_command(cmd);
    ESP_LOGW(TAG, "command %s from web%s", name, force ? " (forced)" : "");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void c3_web_start(void)
{
    build_auth();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 10;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }

    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = get_root };
    const httpd_uri_t state = { .uri = "/api/state", .method = HTTP_GET, .handler = get_state };
    const httpd_uri_t authu = { .uri = "/api/auth", .method = HTTP_GET, .handler = get_auth };
    const httpd_uri_t conf = { .uri = "/api/config", .method = HTTP_POST, .handler = post_config };
    const httpd_uri_t cmd = { .uri = "/api/command", .method = HTTP_POST, .handler = post_command };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &state);
    httpd_register_uri_handler(srv, &authu);
    httpd_register_uri_handler(srv, &conf);
    httpd_register_uri_handler(srv, &cmd);
    ESP_LOGI(TAG, "web UI on http://192.168.4.1/");
}
