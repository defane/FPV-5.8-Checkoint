/* web_auth: signed admin requests for the C3/C6 web pages. Pure-C core
 * (web_auth.c, host-tested in test/test_web_auth.c); ESP glue in web_auth_esp.h,
 * browser side in www/auth.js.
 *
 * The key is K = PBKDF2-HMAC-SHA256(password, salt, iterations), derived at build
 * time (project_include.cmake -> admin_hash.h) and by the page at login; the
 * password never leaves the browser. The page opens a session (a random 128-bit
 * id, POST /api/session, which also returns the salt and iterations) and signs
 * every admin request:
 *   X-Auth-Session: sid   (WA_SID_LEN lowercase hex)
 *   X-Auth-Seq:     seq   (decimal 1, 2, 3...: one more per request)
 *   X-Auth-Sig:     hex(HMAC-SHA256(K, sid "\n" seq "\n" method "\n" uri "\n" body))
 * A session accepts each seq once and only in increasing order, so a captured
 * request replayed, or one with a changed body, is refused. Sessions live in RAM
 * (WA_SESSIONS, the least recently used evicted), so a reboot drops them; the
 * page then reopens one with the same K without asking for the password.
 * A captured request still lets a listener test password guesses offline, each
 * costing the PBKDF2 iterations: only a long random password resists that. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WA_KEY_LEN      32   /* bytes: SHA-256 */
#define WA_SID_LEN      32   /* hex chars: 16 random bytes */
#define WA_SEQ_MAX_LEN  10   /* decimal digits of a uint32 */
#define WA_SIG_LEN      64   /* hex chars: HMAC-SHA256 */
#define WA_SESSIONS     4

typedef struct {
    char sid[WA_SID_LEN + 1];
    uint32_t seq;    /* last accepted, 0 = none yet */
    uint32_t used;   /* LRU stamp, 0 = free slot */
} wa_session_t;

typedef struct {
    wa_session_t s[WA_SESSIONS];
    uint32_t clock;  /* LRU counter */
} wa_table_t;

/* HMAC-SHA256(K, parts[0] || ... || parts[n-1]) into mac (a part may be empty);
 * false on failure. */
typedef bool (*wa_mac_fn)(void *ctx, const uint8_t *const parts[], const size_t lens[],
                          size_t n, uint8_t mac[32]);

typedef enum {
    WA_OK = 0,
    WA_MALFORMED,   /* an X-Auth-* value missing or ill-formed */
    WA_NO_SESSION,  /* unknown session (evicted, or the ESP rebooted): open another */
    WA_REPLAY,      /* seq not above the session's last accepted one */
    WA_BAD_MAC,     /* wrong key (password) or altered request */
} wa_result_t;

void wa_init(wa_table_t *t);

/* Opens a session from 16 random bytes in a free slot, else the least recently
 * used one; returns its id (WA_SID_LEN hex chars), valid until evicted. */
const char *wa_open(wa_table_t *t, const uint8_t rnd[16]);

/* Checks one request. sid/seq/sig are the header values, NULL when absent; body
 * may be NULL when len is 0. On WA_OK the session's seq and LRU stamp advance;
 * any other result leaves the table unchanged. */
wa_result_t wa_verify(wa_table_t *t, const char *sid, const char *seq, const char *sig,
                      const char *method, const char *uri, const uint8_t *body, size_t len,
                      wa_mac_fn mac, void *ctx);
