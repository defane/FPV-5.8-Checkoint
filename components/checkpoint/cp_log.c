/* See cp_log.h. Producers are the scan, console and link tasks; the consumer is the link task. */
#include "cp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"

#define LOG_RING   2048u   /* bytes, power of two */
#define LOG_LINE   192u    /* a longer line is cut in the mirror (not on the console) */

static volatile bool s_on;
static char *s_ring;
static uint32_t s_head, s_tail, s_lost;   /* free-running; used = head - tail */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

bool cp_log_start(void)
{
    if (s_ring) return true;
    char *r = heap_caps_malloc(LOG_RING, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!r) return false;
    portENTER_CRITICAL(&s_mux);
    s_head = s_tail = s_lost = 0;
    s_ring = r;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void cp_log_enable(bool on)
{
    portENTER_CRITICAL(&s_mux);
    if (on != s_on) {            /* the bridge repeats "on" every second: only a change clears */
        s_on = on;
        s_head = s_tail = s_lost = 0;
    }
    portEXIT_CRITICAL(&s_mux);
}

bool cp_log_enabled(void) { return s_on; }

static void push(const char *s, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    if (s_ring) {
        if (LOG_RING - (s_head - s_tail) < n) {
            s_lost += (uint32_t)n;
        } else {
            for (size_t i = 0; i < n; i++) s_ring[(s_head + i) & (LOG_RING - 1u)] = s[i];
            s_head += (uint32_t)n;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

int cp_printf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = vprintf(fmt, ap);            /* the console gets the whole line */
    va_end(ap);
    if (s_on && s_ring && n > 0) {
        char line[LOG_LINE];
        int m = vsnprintf(line, sizeof line, fmt, ap2);
        if (m >= (int)sizeof line) { m = (int)sizeof line - 1; line[m - 1] = '\n'; }   /* cut: keep it a line */
        if (m > 0) push(line, (size_t)m);
    }
    va_end(ap2);
    return n;
}

size_t cp_log_take(char *out, size_t max, uint32_t *lost)
{
    size_t n = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_ring) {
        const uint32_t used = s_head - s_tail;
        n = used < max ? used : max;
        for (size_t i = 0; i < n; i++) out[i] = s_ring[(s_tail + i) & (LOG_RING - 1u)];
        s_tail += (uint32_t)n;
    }
    if (lost) { *lost = s_lost; s_lost = 0; }
    portEXIT_CRITICAL(&s_mux);
    return n;
}
