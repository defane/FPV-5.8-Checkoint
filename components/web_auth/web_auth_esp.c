/* web_auth ESP glue (web_auth_esp.h): PSA HMAC-SHA256, /api/session, /auth.js. */
#include "web_auth.h"
#include "web_auth_esp.h"
#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "web_auth";

extern const uint8_t auth_js_start[] asm("_binary_auth_js_start");
extern const uint8_t auth_js_end[]   asm("_binary_auth_js_end");

static wa_table_t s_table;
static psa_key_id_t s_key;
static bool s_key_ok;      /* imported: false locks every admin action */
static bool s_open;
static const char *s_salt = "";
static uint32_t s_iterations;

static bool mac(void *ctx, const uint8_t *const parts[], const size_t lens[], size_t n,
                uint8_t out[32])
{
    (void)ctx;
    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    size_t olen = 0;
    if (psa_mac_sign_setup(&op, s_key, PSA_ALG_HMAC(PSA_ALG_SHA_256)) != PSA_SUCCESS) return false;
    for (size_t i = 0; i < n; i++)
        if (lens[i] && psa_mac_update(&op, parts[i], lens[i]) != PSA_SUCCESS) {
            psa_mac_abort(&op);
            return false;
        }
    if (psa_mac_sign_finish(&op, out, 32, &olen) != PSA_SUCCESS || olen != 32) {
        psa_mac_abort(&op);
        return false;
    }
    return true;
}

void web_auth_init(const uint8_t key[32], const char *salt, uint32_t iterations, bool open)
{
    wa_init(&s_table);
    s_salt = salt;
    s_iterations = iterations;
    s_open = open;
    if (open) return;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&a, 8 * WA_KEY_LEN);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    s_key_ok = psa_import_key(&a, key, WA_KEY_LEN, &s_key) == PSA_SUCCESS;
    if (!s_key_ok) ESP_LOGE(TAG, "HMAC key import failed: admin actions locked");
}

static esp_err_t post_session(httpd_req_t *req)
{
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof rnd);   /* hardware RNG, true random while Wi-Fi runs */
    char out[160];
    const int o = snprintf(out, sizeof out, "{\"sid\":\"%s\",\"salt\":\"%s\",\"iter\":%lu,\"open\":%s}",
                           wa_open(&s_table, rnd), s_salt, (unsigned long)s_iterations,
                           s_open ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, o);
}

static esp_err_t get_js(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req, (const char *)auth_js_start, auth_js_end - auth_js_start);
}

void web_auth_register(httpd_handle_t srv)
{
    const httpd_uri_t session = { .uri = "/api/session", .method = HTTP_POST, .handler = post_session };
    const httpd_uri_t js = { .uri = "/auth.js", .method = HTTP_GET, .handler = get_js };
    httpd_register_uri_handler(srv, &session);
    httpd_register_uri_handler(srv, &js);
}

bool web_auth_request(httpd_req_t *req, char *buf, size_t cap)
{
    const int total = req->content_len;
    if (total < 0 || (size_t)total >= cap) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return false;
    }
    for (int got = 0; got < total;) {
        const int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return false; }
        got += r;
    }
    buf[total] = '\0';
    if (s_open) return true;

    /* One spare byte each: ESP-IDF truncates a value exactly the buffer's size and
     * still returns ESP_OK; a longer value then fails the exact-length checks. */
    char sid[WA_SID_LEN + 2], seq[WA_SEQ_MAX_LEN + 2], sig[WA_SIG_LEN + 2];
    const bool has_sid = httpd_req_get_hdr_value_str(req, "X-Auth-Session", sid, sizeof sid) == ESP_OK;
    const bool has_seq = httpd_req_get_hdr_value_str(req, "X-Auth-Seq", seq, sizeof seq) == ESP_OK;
    const bool has_sig = httpd_req_get_hdr_value_str(req, "X-Auth-Sig", sig, sizeof sig) == ESP_OK;
    const wa_result_t r = !s_key_ok ? WA_BAD_MAC
        : wa_verify(&s_table, has_sid ? sid : NULL, has_seq ? seq : NULL, has_sig ? sig : NULL,
                    http_method_str(req->method), req->uri, (const uint8_t *)buf, (size_t)total,
                    mac, NULL);
    if (r == WA_OK) return true;
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, r == WA_NO_SESSION ? "{\"ok\":false,\"auth\":true,\"session\":false}"
                                               : "{\"ok\":false,\"auth\":true}");
    return false;
}
