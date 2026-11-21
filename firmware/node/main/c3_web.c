#include "c3_web.h"
#include "admin_hash.h"
#include "c3_link.h"
#include "c3_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "web_auth_esp.h"
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

/* One level per Raceband channel, dB x10, LP_CN_UNKNOWN when not measured. */
static void put_levels(char *b, size_t cap, const int8_t *levels)
{
    int o = 0;
    for (unsigned ch = 0; ch < LP_LEVELS; ch++) {
        const bool known = levels && levels[ch] != LP_LEVEL_UNKNOWN;
        o += snprintf(b + o, cap - o, "%s%d", ch ? "," : "", known ? levels[ch] * 10 : LP_CN_UNKNOWN);
    }
}

/* Each channel's detector state (pd_state_t, 0..3), R1 first: "0,3,..." (15 B). */
static void put_states(char *b, size_t cap, uint16_t states)
{
    int o = 0;
    for (unsigned ch = 0; ch < LP_LEVELS; ch++)
        o += snprintf(b + o, cap - o, "%s%u", ch ? "," : "", lp_state_of(states, ch));
}

static esp_err_t get_state(httpd_req_t *req)
{
    c3_snapshot_t s;
    c3_store_snapshot(&s);
    char fw[LP_FW_LEN], levels[48], states[16];   /* levels 8 x "-1270" + 7 commas = 47 B */
    json_safe(fw, s.stats.fw, sizeof fw);
    /* A silent C5's last levels and states are stale: unknown and IDLE, as the C6 shows one. */
    const bool alive = c3_link_c5_alive(), live = s.has_stats && alive;
    put_levels(levels, sizeof levels, live ? s.stats.levels : NULL);
    put_states(states, sizeof states, live ? s.stats.states : 0u);

    char body[2048];   /* worst case (16 events, every field at its widest): 1620 B */
    int o = 0;
    o += snprintf(body + o, sizeof body - o,
                  "{\"has_config\":%s,\"config\":{\"alpha_milli\":%ld,\"active_db_x10\":%ld,"
                  "\"hysteresis_db_x10\":%ld,\"drop_db_x10\":%ld,\"min_peak_db_x10\":%ld,"
                  "\"refractory_ms\":%lu,\"stale_ms\":%lu,\"rearm_db_x10\":%ld,\"volume_pct\":%u,\"muted\":%u},",
                  s.has_config ? "true" : "false",
                  (long)s.config.alpha_milli, (long)s.config.active_db_x10,
                  (long)s.config.hysteresis_db_x10, (long)s.config.drop_db_x10,
                  (long)s.config.min_peak_db_x10, (unsigned long)s.config.refractory_ms,
                  (unsigned long)s.config.stale_ms, (long)s.config.rearm_db_x10, s.config.volume_pct,
                  s.config.muted);
    o += snprintf(body + o, sizeof body - o,
                  "\"c5_alive\":%s,"
                  "\"has_stats\":%s,\"stats\":{\"cn_x10\":%ld,\"states\":[%s],"
                  "\"flags\":%u,\"cal_needed\":%s,\"calibrating\":%s,\"gain\":%u,\"passes\":%lu,"
                  "\"uptime_s\":%lu,\"heap_min\":%lu,\"faults\":%lu,\"fw\":\"%s\",\"levels\":[%s]},"
                  "\"total_events\":%lu,\"events\":[",
                  alive ? "true" : "false",
                  s.has_stats ? "true" : "false", (long)s.stats.cn_x10, states, s.stats.flags,
                  (s.stats.flags & LP_HEALTH_CAL_NEEDED) ? "true" : "false",
                  (s.stats.flags & LP_HEALTH_CALIBRATING) ? "true" : "false", s.stats.gain,
                  (unsigned long)s.stats.passes, (unsigned long)s.stats.uptime_s,
                  (unsigned long)s.stats.heap_min, (unsigned long)s.stats.faults, fw, levels,
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

/* ---- admin auth (spectators read results; config and commands are signed) ---- */
/* Every admin request is signed in the page with K = PBKDF2(password, ADMIN_SALT,
 * ADMIN_ITERATIONS), the only form of the password the firmware holds (admin_hash.h,
 * generated at build time by web_auth_admin_key()); components/web_auth checks the signature,
 * the session and its replay counter. web_auth_request() also reads the body:
 * on false its 400/401 is already sent. */
static const uint8_t s_admin_key[] = ADMIN_KEY;

static esp_err_t get_auth(httpd_req_t *req)
{
    char buf[8];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"admin\":true}");
}

/* The page posts every detector field: 8 of them, about 330 B with every number at its widest
 * JSON form (test/test_web_mock.py checks it against this buffer). A value the C5 refuses (out of
 * range, the re-arm drop's 0..LP_REARM_MAX_X10 included) leaves it on its settings: its echo
 * (CONFIG_STATE) replaces the stored copy, and the page shows it. */
static esp_err_t post_config(httpd_req_t *req)
{
    char buf[512];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;

    lp_config_t cur;
    (void)c3_store_get_config(&cur);           /* start from current, override given keys */
    cur.alpha_milli       = json_i(buf, "alpha_milli", cur.alpha_milli);
    cur.active_db_x10     = json_i(buf, "active_db_x10", cur.active_db_x10);
    cur.hysteresis_db_x10 = json_i(buf, "hysteresis_db_x10", cur.hysteresis_db_x10);
    cur.drop_db_x10       = json_i(buf, "drop_db_x10", cur.drop_db_x10);
    cur.min_peak_db_x10   = json_i(buf, "min_peak_db_x10", cur.min_peak_db_x10);
    cur.refractory_ms     = (uint32_t)json_i(buf, "refractory_ms", (int32_t)cur.refractory_ms);
    cur.stale_ms          = (uint32_t)json_i(buf, "stale_ms", (int32_t)cur.stale_ms);
    cur.rearm_db_x10      = json_i(buf, "rearm_db_x10", cur.rearm_db_x10);
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

/* What the console's B, F and a reset do, for a C5 nobody plugs USB into:
 * {"cmd":"cal_full"|"cal_quick"|"reboot","force":0|1}. A calibration blinds
 * the gate and wants the VTXs off, so unless forced it is refused while the
 * C5 sees a carrier above the activity threshold (a VTX is probably on); the
 * page then asks the admin and retries with force. Any command is refused,
 * forced or not, while no C5 is linked: nothing would run it. */
static esp_err_t post_command(httpd_req_t *req)
{
    char buf[96], name[16];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
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

/* Debug mode (admin): the C5's console text and typed keys. POST {"since":N} answers
 * {"next":M,"lost":0|1,"text":"..."}: the bytes after position N (c3_link_log_read; the text
 * holds no quote, backslash or control other than the newline, which is escaped here). The page
 * asks again from "next". */
static esp_err_t post_log(httpd_req_t *req)
{
    char buf[64];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    char raw[C3_LOG_CHUNK];
    uint32_t next;
    bool lost;
    const size_t n = c3_link_log_read((uint32_t)json_i(buf, "since", 0), raw, sizeof raw, &next, &lost);

    char body[2 * C3_LOG_CHUNK + 96];
    int o = snprintf(body, sizeof body, "{\"debug\":%u,\"next\":%lu,\"lost\":%u,\"c5_alive\":%u,\"text\":\"",
                     c3_link_debug() ? 1u : 0u, (unsigned long)next, lost ? 1u : 0u, c3_link_c5_alive() ? 1u : 0u);
    for (size_t i = 0; i < n; i++) {
        if (raw[i] == '\n') { body[o++] = '\\'; body[o++] = 'n'; }
        else body[o++] = raw[i];
    }
    o += snprintf(body + o, sizeof body - o, "\"}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, o);
}

/* Debug mode on or off ({"on":0|1}; no "on" only asks). Off at every boot: nothing is mirrored
 * from the C5, and /api/key sends nothing, until an admin switches it on. */
static esp_err_t post_debug(httpd_req_t *req)
{
    char buf[32];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    const int32_t on = json_i(buf, "on", -1);
    if (on >= 0) {
        c3_link_debug_set(on != 0);
        ESP_LOGW(TAG, "debug mode %s from web", on ? "ON" : "off");
    }
    char r[32];
    snprintf(r, sizeof r, "{\"ok\":true,\"debug\":%u}", c3_link_debug() ? 1u : 0u);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, r);
}

/* {"keys":"Ka","force":0|1}: typed on the C5's console as on its USB ("K" opens the settings
 * menu, "T" is the status, "F"/"B" calibrate). The keys that blind the gate follow
 * post_command's rule: refused with {"blind":true}, or {"vtx":true} while a carrier is seen,
 * until the page asks the admin and retries with force. The C5's answer comes back through
 * /api/log. */
static esp_err_t post_key(httpd_req_t *req)
{
    char buf[96], keys[17];
    if (!web_auth_request(req, buf, sizeof buf)) return ESP_OK;
    if (!json_s(buf, "keys", keys, sizeof keys) || !keys[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no keys");
        return ESP_FAIL;
    }
    if (!lp_keys_valid(keys)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad key");
        return ESP_FAIL;
    }
    const bool force = json_i(buf, "force", 0) != 0;

    httpd_resp_set_type(req, "application/json");
    if (!c3_link_debug())
        return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"debug mode is off\"}");
    if (!c3_link_c5_alive())
        return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"no C5 linked\"}");
    if (lp_keys_blind(keys) && !force) {
        c3_snapshot_t s;
        c3_store_snapshot(&s);
        char r[72];
        if (s.has_stats && s.stats.cn_x10 != LP_CN_UNKNOWN && s.stats.cn_x10 > s.config.active_db_x10)
            snprintf(r, sizeof r, "{\"ok\":false,\"vtx\":true,\"cn_x10\":%ld}", (long)s.stats.cn_x10);
        else
            snprintf(r, sizeof r, "{\"ok\":false,\"blind\":true}");
        return httpd_resp_sendstr(req, r);
    }
    c3_link_send_keys(keys, strlen(keys));
    ESP_LOGW(TAG, "keys \"%s\" from web%s", keys, force ? " (forced)" : "");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void c3_web_start(void)
{
    web_auth_init(s_admin_key, ADMIN_SALT, ADMIN_ITERATIONS, ADMIN_AUTH_OPEN);
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 10;
    cfg.stack_size = 6144;       /* the signature's PSA HMAC under the handlers: ~3.5 KB of the 4 KB default */
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
    const httpd_uri_t logu = { .uri = "/api/log", .method = HTTP_POST, .handler = post_log };
    const httpd_uri_t keyu = { .uri = "/api/key", .method = HTTP_POST, .handler = post_key };
    httpd_register_uri_handler(srv, &logu);
    httpd_register_uri_handler(srv, &keyu);
    const httpd_uri_t dbgu = { .uri = "/api/debug", .method = HTTP_POST, .handler = post_debug };
    httpd_register_uri_handler(srv, &dbgu);
    web_auth_register(srv);   /* POST /api/session, GET /auth.js */
    ESP_LOGI(TAG, "web UI on http://192.168.4.1/");
}
