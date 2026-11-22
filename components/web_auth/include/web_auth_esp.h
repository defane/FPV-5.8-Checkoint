/* web_auth ESP glue: the C3/C6 admin web's signed requests (protocol in web_auth.h).
 * HMAC-SHA256 through PSA (hardware SHA), session ids from the hardware RNG. */
#pragma once
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* key: K = PBKDF2-HMAC-SHA256(password, salt, iterations) (admin_hash.h), salt and
 * iterations passed on to the page; open: empty password, every request is admin.
 * Call once, before web_auth_register(). */
void web_auth_init(const uint8_t key[32], const char *salt, uint32_t iterations, bool open);

/* Registers POST /api/session (opens a session: {"sid","salt","iter","open"}) and
 * GET /auth.js (the page side): two URI handlers. */
void web_auth_register(httpd_handle_t srv);

/* An admin handler's first call: reads the body into buf[cap] (NUL-terminated,
 * possibly empty) and checks the request's signature. On false the 400 or 401
 * reply is sent (401 {"session":false}: unknown session, the page reopens one)
 * and the handler returns ESP_OK. */
bool web_auth_request(httpd_req_t *req, char *buf, size_t cap);

/* The headers every page and script of the C3/C6 carries: a Content-Security-Policy that lets the page run its own
 * scripts (/app.js and /auth.js, 'self'; no inline script, no eval) and talk to its own origin only, with nothing
 * framed, no form posted, no base URL, and no sniffing of the type or leaking of the referrer. Styles may be inline
 * (the pages set style attributes): a style cannot run code. The pages' HTML therefore holds no inline script and no
 * on...= attribute (test/test_web_mock.py checks it). */
#define WA_CSP "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'; " \
               "img-src 'self' data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"
void web_auth_page_headers(httpd_req_t *req);   /* sets them on the reply; call before httpd_resp_send */
