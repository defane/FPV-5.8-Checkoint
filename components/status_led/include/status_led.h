/* status_led: a single-WS2812 status indicator.
 *
 * Two layers:
 *   - this pure-C core (no ESP headers): a state -> colour/pattern machine,
 *     host-tested in test/test_status_led.c;
 *   - status_led_esp.c (ESP glue): a bit-banged-over-RMT WS2812 driver and a
 *     priority-1 task that calls sled_tick() every ~50 ms.
 *
 * The firmware pushes its state through the runtime setters at the bottom; the
 * core turns that into an {r,g,b} for the current instant. All timing is
 * wrap-safe (unsigned millisecond deltas).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SLED_BOOT = 0,   /* initializing / self-check   -> yellow blink           */
    SLED_HEALTHY,    /* normal, connected/operating -> green breathing        */
    SLED_DEGRADED,   /* functional fallback         -> blue breathing         */
    SLED_PAIRING,    /* waiting to pair (fleet)     -> magenta double-pulse    */
    SLED_ERR_RECOV,  /* recoverable (link/peer lost)-> red fast blink          */
    SLED_ERR_FATAL,  /* unrecoverable (init/pin/NVS)-> red solid               */
} sled_base_t;

typedef struct { uint8_t r, g, b; } sled_rgb_t;

/* Opaque-ish core state. Heap-allocate it on the target (RAM is tight); never
 * add it as static .bss. Fields are exposed only so the host test and the glue
 * can embed it by value. */
typedef struct {
    sled_base_t base;
    uint8_t     brightness;    /* 0..255 master scale                         */
    uint8_t     quality;       /* 0..255 bench C/N level (optional)           */
    bool        quality_on;    /* true: HEALTHY brightness tracks `quality`   */
    bool        pass_active;   /* a pass flash is in progress                 */
    uint32_t    pass_t0_ms;    /* when the pass flash started                 */
    uint32_t    t0_ms;         /* init time; all pattern phases are relative  */
} sled_t;

void       sled_init(sled_t *s, uint32_t now_ms);     /* base=BOOT, 15% default */
void       sled_set_base(sled_t *s, sled_base_t base);
void       sled_set_brightness(sled_t *s, uint8_t brightness);
void       sled_pass(sled_t *s, uint32_t now_ms);     /* latch a pass flash     */
void       sled_set_quality(sled_t *s, uint8_t q);    /* enable bench C/N level */
void       sled_clear_quality(sled_t *s);
sled_rgb_t sled_tick(sled_t *s, uint32_t now_ms);     /* colour for `now_ms`    */

/* ---- ESP runtime (status_led_esp.c); not compiled by the host test ---- *
 * Thread-safe setters the firmware calls from any task; a priority-1 task
 * renders the core every CONFIG_STATUS_LED_TICK_MS. All are no-ops when
 * CONFIG_STATUS_LED_ENABLE=n, so callers need no #ifdef. */
void status_led_start(void);                    /* create WS2812 driver + task  */
void status_led_set_base(sled_base_t base);
void status_led_pass(void);                     /* latch a pass flash           */
void status_led_set_quality_cn_x10(int cn_x10); /* bench C/N; <0 clears          */
