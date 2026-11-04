/* Host test for the web_auth core (sessions, replay window, signature check).
 * The MAC is a reference HMAC-SHA256 here (PSA on the ESP); the vectors come from
 * Python's hmac module and are shared with test/test_web_auth.mjs. Build:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/web_auth/include \
 *     test_web_auth.c ../components/web_auth/web_auth.c -o /tmp/t && /tmp/t
 */
#include "web_auth.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(name, cond) do { \
    if (cond) { printf("ok: %s\n", (name)); } \
    else { printf("FAIL: %s\n", (name)); fails++; } } while (0)

/* ---- reference SHA-256 / HMAC-SHA256 (FIPS 180-4, RFC 2104) ---- */
typedef struct { uint32_t h[8]; uint8_t buf[64]; size_t n; uint64_t total; } sha_t;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void sha_block(sha_t *s, const uint8_t *p)
{
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
               (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
    memcpy(v, s->h, sizeof v);
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = v[7] + (ROR(v[4], 6) ^ ROR(v[4], 11) ^ ROR(v[4], 25)) +
                            ((v[4] & v[5]) ^ (~v[4] & v[6])) + K[i] + w[i];
        const uint32_t t2 = (ROR(v[0], 2) ^ ROR(v[0], 13) ^ ROR(v[0], 22)) +
                            ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof v[0]);
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += v[i];
}

static void sha_init(sha_t *s)
{
    static const uint32_t h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, h0, sizeof h0);
    s->n = 0;
    s->total = 0;
}

static void sha_update(sha_t *s, const uint8_t *p, size_t len)
{
    s->total += len;
    while (len--) {
        s->buf[s->n++] = *p++;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}

static void sha_final(sha_t *s, uint8_t out[32])
{
    const uint64_t bits = s->total * 8;
    const uint8_t one = 0x80, zero = 0;
    sha_update(s, &one, 1);
    while (s->n != 56) sha_update(s, &zero, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, l, 8);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(s->h[i] >> (24 - 8 * j));
}

static bool ref_mac(void *ctx, const uint8_t *const parts[], const size_t lens[], size_t n,
                    uint8_t mac[32])
{
    const uint8_t *key = ctx;   /* WA_KEY_LEN bytes, shorter than a block */
    uint8_t pad[64], ih[32];
    sha_t s;
    memset(pad, 0x36, sizeof pad);
    for (int i = 0; i < WA_KEY_LEN; i++) pad[i] ^= key[i];
    sha_init(&s);
    sha_update(&s, pad, 64);
    for (size_t i = 0; i < n; i++) sha_update(&s, parts[i], lens[i]);
    sha_final(&s, ih);
    memset(pad, 0x5c, sizeof pad);
    for (int i = 0; i < WA_KEY_LEN; i++) pad[i] ^= key[i];
    sha_init(&s);
    sha_update(&s, pad, 64);
    sha_update(&s, ih, 32);
    sha_final(&s, mac);
    return true;
}

static bool fail_mac(void *ctx, const uint8_t *const parts[], const size_t lens[], size_t n,
                     uint8_t mac[32])
{
    (void)ctx; (void)parts; (void)lens; (void)n; (void)mac;
    return false;
}

/* ---- shared vectors (Python: hmac.new(K, msg, sha256)) ---- */
/* Any 32-byte key: the core does not derive K (the build and the page do, PBKDF2);
 * this one is SHA-256("fpv58-checkpoint/c6:checkpoint"). */
static const uint8_t KEY[32] = {
    0x13, 0xa6, 0x66, 0x09, 0xc4, 0x4b, 0x40, 0x55, 0x0b, 0x8e, 0xa5, 0xce, 0x65, 0x5b, 0x11, 0x6e,
    0x95, 0x43, 0x43, 0x9a, 0x78, 0x70, 0x30, 0x1f, 0xc9, 0x4e, 0xdf, 0x5b, 0x21, 0x7c, 0x53, 0x0e,
};
static const uint8_t RND[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
#define SID "00112233445566778899aabbccddeeff"
#define SIG1 "4ca74e9d9a99af7f5f9a48e8cec40d2e2892d8541214aeb11a5305087ea9c40e"  /* 1 POST /api/reset {} */
#define SIG2 "391ec91e5996b3a933f648d53cc00156fa0ce40b43859d7b6e358d4d58b62a3d"  /* 2 GET /api/auth "" */
#define BODY3 "{\"node_id\":1,\"gate\":2,\"channel\":3}"
#define SIG3 "ef7f3361727f8339a720e97a887a73fab0b78cc74b778d36db241b504e79aacf"  /* 3 POST /api/node BODY3 */

static wa_result_t req(wa_table_t *t, const char *sid, const char *seq, const char *sig,
                       const char *method, const char *uri, const char *body)
{
    return wa_verify(t, sid, seq, sig, method, uri, (const uint8_t *)body, body ? strlen(body) : 0,
                     ref_mac, (void *)KEY);
}

int main(void)
{
    wa_table_t t;
    wa_init(&t);

    /* Reference SHA-256 sanity ("abc", FIPS 180-4 example). */
    sha_t s;
    uint8_t d[32];
    sha_init(&s);
    sha_update(&s, (const uint8_t *)"abc", 3);
    sha_final(&s, d);
    CHECK("reference sha256(abc)", d[0] == 0xba && d[1] == 0x78 && d[31] == 0xad);

    const char *sid = wa_open(&t, RND);
    CHECK("sid is the random bytes in lowercase hex", strcmp(sid, SID) == 0);

    /* Python vectors, in order. */
    CHECK("vector 1 accepted", req(&t, SID, "1", SIG1, "POST", "/api/reset", "{}") == WA_OK);
    CHECK("vector 2 accepted (empty body)", req(&t, SID, "2", SIG2, "GET", "/api/auth", "") == WA_OK);
    CHECK("NULL body equals empty body", wa_verify(&t, SID, "2", SIG2, "GET", "/api/auth", NULL, 0,
                                                   ref_mac, (void *)KEY) == WA_REPLAY);
    CHECK("vector 3 accepted", req(&t, SID, "3", SIG3, "POST", "/api/node", BODY3) == WA_OK);

    /* Replays and order. */
    CHECK("replay of seq 3 refused", req(&t, SID, "3", SIG3, "POST", "/api/node", BODY3) == WA_REPLAY);
    CHECK("older seq refused", req(&t, SID, "1", SIG1, "POST", "/api/reset", "{}") == WA_REPLAY);
    CHECK("session seq is 3", t.s[0].seq == 3);

    /* Fresh session on the same slot table: the vectors are tied to seq, not reusable elsewhere. */
    wa_table_t u;
    wa_init(&u);
    (void)wa_open(&u, RND);
    CHECK("altered body refused", req(&u, SID, "3", SIG3, "POST", "/api/node", "{\"node_id\":1,\"gate\":9,\"channel\":3}") == WA_BAD_MAC);
    CHECK("other path refused", req(&u, SID, "1", SIG1, "POST", "/api/pairing", "{}") == WA_BAD_MAC);
    CHECK("other method refused", req(&u, SID, "1", SIG1, "GET", "/api/reset", "{}") == WA_BAD_MAC);
    CHECK("other seq refused (sig bound to seq)", req(&u, SID, "4", SIG1, "POST", "/api/reset", "{}") == WA_BAD_MAC);
    CHECK("bad MAC leaves seq untouched", u.s[0].seq == 0);
    CHECK("MAC failure refuses", wa_verify(&u, SID, "1", SIG1, "POST", "/api/reset", (const uint8_t *)"{}", 2,
                                           fail_mac, NULL) == WA_BAD_MAC);
    CHECK("seq gap allowed upward", req(&u, SID, "3", SIG3, "POST", "/api/node", BODY3) == WA_OK);
    CHECK("then lower seq refused", req(&u, SID, "2", SIG2, "GET", "/api/auth", "") == WA_REPLAY);

    /* Malformed headers. */
    wa_init(&u);
    (void)wa_open(&u, RND);
    CHECK("missing sid", req(&u, NULL, "1", SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("missing seq", req(&u, SID, NULL, SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("missing sig", req(&u, SID, "1", NULL, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("sid too short", req(&u, "0011", "1", SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("sid too long", req(&u, SID "0", "1", SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("sid uppercase", req(&u, "00112233445566778899AABBCCDDEEFF", "1", SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("seq 0", req(&u, SID, "0", SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("seq leading zero", req(&u, SID, "01", SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("seq sign", req(&u, SID, "+1", SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("seq not a number", req(&u, SID, "1a", SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("seq over uint32", req(&u, SID, "4294967296", SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("seq 11 digits", req(&u, SID, "12345678901", SIG1, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("sig too short", req(&u, SID, "1", "4ca7", "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("sig too long", req(&u, SID, "1", SIG1 "0", "POST", "/api/reset", "{}") == WA_MALFORMED);
    char nothex[] = SIG1;
    nothex[0] = 'z';
    CHECK("sig not hex", req(&u, SID, "1", nothex, "POST", "/api/reset", "{}") == WA_MALFORMED);
    nothex[0] = 'A';
    CHECK("sig uppercase", req(&u, SID, "1", nothex, "POST", "/api/reset", "{}") == WA_MALFORMED);
    CHECK("body NULL with a length", wa_verify(&u, SID, "1", SIG1, "POST", "/api/reset", NULL, 2,
                                               ref_mac, (void *)KEY) == WA_MALFORMED);
    CHECK("malformed leaves seq untouched", u.s[0].seq == 0);
    CHECK("seq max uint32 parses", req(&u, SID, "4294967295", SIG1, "POST", "/api/reset", "{}") == WA_BAD_MAC);

    /* Sessions: unknown id, LRU eviction. */
    CHECK("unknown session", req(&u, "ffeeddccbbaa99887766554433221100", "1", SIG1, "POST", "/api/reset", "{}") == WA_NO_SESSION);
    wa_init(&u);
    uint8_t r[16] = { 0 };
    char ids[WA_SESSIONS + 1][WA_SID_LEN + 1];
    for (unsigned i = 0; i <= WA_SESSIONS; i++) {
        r[15] = (uint8_t)(0xa0 + i);
        strcpy(ids[i], wa_open(&u, r));
        if (i == 1) {                    /* use session 0 so session 1 becomes the oldest */
            memcpy(u.s[0].sid, SID, sizeof u.s[0].sid);
            (void)req(&u, SID, "1", SIG1, "POST", "/api/reset", "{}");
        }
    }
    bool has1 = false, has0 = false, has_last = false;
    for (unsigned i = 0; i < WA_SESSIONS; i++) {
        has0 |= strcmp(u.s[i].sid, SID) == 0;
        has1 |= strcmp(u.s[i].sid, ids[1]) == 0;
        has_last |= strcmp(u.s[i].sid, ids[WA_SESSIONS]) == 0;
    }
    CHECK("full table evicts the least recently used", !has1 && has0 && has_last);
    CHECK("a new session starts at seq 0", req(&u, ids[WA_SESSIONS], "1", SIG1, "POST", "/api/reset", "{}") == WA_BAD_MAC);

    if (fails) { printf("%d test(s) FAILED\n", fails); return 1; }
    printf("all web_auth tests passed\n");
    return 0;
}
