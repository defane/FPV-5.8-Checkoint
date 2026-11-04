/* web_auth core: sessions, replay window and signature check (web_auth.h). */
#include "web_auth.h"
#include <string.h>

static const char k_hex[] = "0123456789abcdef";

/* Lowercase only: one spelling per value. */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool is_hex(const char *s, size_t n)
{
    if (!s || strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (hexval(s[i]) < 0) return false;
    return true;
}

/* Decimal 1..UINT32_MAX: no sign, no leading zero. */
static bool parse_seq(const char *s, uint32_t *out)
{
    if (!s || s[0] < '1' || s[0] > '9') return false;
    uint64_t v = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9' || i >= WA_SEQ_MAX_LEN) return false;
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    if (v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

static uint32_t tick(wa_table_t *t)
{
    if (++t->clock == 0) t->clock = 1;   /* 0 marks a free slot */
    return t->clock;
}

void wa_init(wa_table_t *t)
{
    memset(t, 0, sizeof *t);
}

const char *wa_open(wa_table_t *t, const uint8_t rnd[16])
{
    wa_session_t *v = &t->s[0];
    for (unsigned i = 0; i < WA_SESSIONS; i++) {
        if (!t->s[i].used) { v = &t->s[i]; break; }
        if (t->s[i].used < v->used) v = &t->s[i];
    }
    for (unsigned i = 0; i < 16; i++) {
        v->sid[2 * i]     = k_hex[rnd[i] >> 4];
        v->sid[2 * i + 1] = k_hex[rnd[i] & 15u];
    }
    v->sid[WA_SID_LEN] = '\0';
    v->seq = 0;
    v->used = tick(t);
    return v->sid;
}

wa_result_t wa_verify(wa_table_t *t, const char *sid, const char *seq, const char *sig,
                      const char *method, const char *uri, const uint8_t *body, size_t len,
                      wa_mac_fn mac, void *ctx)
{
    uint32_t n;
    if (!is_hex(sid, WA_SID_LEN) || !parse_seq(seq, &n) || !is_hex(sig, WA_SIG_LEN) ||
        !method || !uri || (!body && len))
        return WA_MALFORMED;
    wa_session_t *s = NULL;
    for (unsigned i = 0; i < WA_SESSIONS; i++)
        if (t->s[i].used && memcmp(t->s[i].sid, sid, WA_SID_LEN) == 0) s = &t->s[i];
    if (!s) return WA_NO_SESSION;
    if (n <= s->seq) return WA_REPLAY;

    static const uint8_t nl = '\n';
    const uint8_t *const parts[] = {
        (const uint8_t *)sid, &nl, (const uint8_t *)seq, &nl, (const uint8_t *)method, &nl,
        (const uint8_t *)uri, &nl, body ? body : (const uint8_t *)"",
    };
    const size_t lens[] = { WA_SID_LEN, 1, strlen(seq), 1, strlen(method), 1, strlen(uri), 1, len };
    uint8_t want[32];
    if (!mac(ctx, parts, lens, sizeof lens / sizeof lens[0], want)) return WA_BAD_MAC;
    uint8_t diff = 0;   /* constant time over the whole signature */
    for (unsigned i = 0; i < 32; i++)
        diff |= (uint8_t)(want[i] ^ (hexval(sig[2 * i]) << 4 | hexval(sig[2 * i + 1])));
    if (diff) return WA_BAD_MAC;

    s->seq = n;
    s->used = tick(t);
    return WA_OK;
}
