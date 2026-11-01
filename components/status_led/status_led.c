/* status_led core — pure C, host-tested. See status_led.h. */
#include "status_led.h"

/* Pattern timings (ms). */
#define BOOT_HALF_MS        250u   /* yellow square blink, half period        */
#define RECOV_HALF_MS       100u   /* red fast blink, half period             */
#define BREATH_HEALTHY_MS  3000u   /* green breathing, full period            */
#define BREATH_DEGRADED_MS 4000u   /* blue breathing, full period             */
#define PAIR_CYCLE_MS      1500u   /* magenta double-pulse cycle              */
#define PASS_MS             120u   /* white pass flash duration               */

#define FAC_MIN             51u    /* breathing floor (~20% of 256)           */
#define FAC_FULL           256u

/* Base colours at full scale (before pattern factor and master brightness). */
static const sled_rgb_t C_BOOT     = {255, 170,   0};
static const sled_rgb_t C_ERR      = {255,   0,   0};
static const sled_rgb_t C_HEALTHY  = {  0, 255,   0};
static const sled_rgb_t C_DEGRADED = {  0,  60, 255};
static const sled_rgb_t C_PAIR     = {255,   0, 255};
static const sled_rgb_t C_PASS     = {255, 255, 255};
static const sled_rgb_t C_OFF      = {  0,   0,   0};

static sled_rgb_t scale(sled_rgb_t c, uint32_t num, uint32_t den)
{
    if (den == 0u) return C_OFF;
    c.r = (uint8_t)((uint32_t)c.r * num / den);
    c.g = (uint8_t)((uint32_t)c.g * num / den);
    c.b = (uint8_t)((uint32_t)c.b * num / den);
    return c;
}

/* Triangle breathing: FAC_MIN..FAC_FULL..FAC_MIN over `period`. */
static uint32_t breath(uint32_t elapsed, uint32_t period)
{
    uint32_t ph   = elapsed % period;
    uint32_t half = period / 2u;
    uint32_t x    = (ph < half) ? ph : (period - ph);   /* 0..half */
    uint32_t tri  = (half == 0u) ? 0u : (x * FAC_FULL / half);
    return FAC_MIN + tri * (FAC_FULL - FAC_MIN) / FAC_FULL;
}

/* Two short pulses then a gap, over PAIR_CYCLE_MS. */
static uint32_t pair_pulse(uint32_t elapsed)
{
    uint32_t ph = elapsed % PAIR_CYCLE_MS;
    if (ph < 120u)               return FAC_FULL;
    if (ph >= 180u && ph < 300u) return FAC_FULL;
    return 0u;
}

void sled_init(sled_t *s, uint32_t now_ms)
{
    s->base        = SLED_BOOT;
    s->brightness  = 40u;        /* ~15%; the glue overrides from Kconfig     */
    s->quality     = 0u;
    s->quality_on  = false;
    s->pass_active = false;
    s->pass_t0_ms  = 0u;
    s->t0_ms       = now_ms;
}

void sled_set_base(sled_t *s, sled_base_t base) { s->base = base; }
void sled_set_brightness(sled_t *s, uint8_t b)  { s->brightness = b; }
void sled_clear_quality(sled_t *s)              { s->quality_on = false; }

void sled_set_quality(sled_t *s, uint8_t q)
{
    s->quality    = q;
    s->quality_on = true;
}

void sled_pass(sled_t *s, uint32_t now_ms)
{
    s->pass_active = true;
    s->pass_t0_ms  = now_ms;
}

sled_rgb_t sled_tick(sled_t *s, uint32_t now_ms)
{
    /* Pass flash wins over every base state except a fatal error. */
    if (s->pass_active && s->base != SLED_ERR_FATAL) {
        if ((uint32_t)(now_ms - s->pass_t0_ms) < PASS_MS)
            return scale(C_PASS, s->brightness, 255u);
        s->pass_active = false;
    }

    uint32_t elapsed = (uint32_t)(now_ms - s->t0_ms);
    sled_rgb_t c;
    uint32_t fac;

    switch (s->base) {
    case SLED_BOOT:
        c   = C_BOOT;
        fac = (elapsed % (2u * BOOT_HALF_MS) < BOOT_HALF_MS) ? FAC_FULL : 0u;
        break;
    case SLED_ERR_FATAL:
        c = C_ERR; fac = FAC_FULL;
        break;
    case SLED_ERR_RECOV:
        c   = C_ERR;
        fac = (elapsed % (2u * RECOV_HALF_MS) < RECOV_HALF_MS) ? FAC_FULL : 0u;
        break;
    case SLED_HEALTHY:
        c   = C_HEALTHY;
        fac = s->quality_on
              ? (FAC_MIN + (uint32_t)s->quality * (FAC_FULL - FAC_MIN) / 255u)
              : breath(elapsed, BREATH_HEALTHY_MS);
        break;
    case SLED_DEGRADED:
        c = C_DEGRADED; fac = breath(elapsed, BREATH_DEGRADED_MS);
        break;
    case SLED_PAIRING:
        c = C_PAIR; fac = pair_pulse(elapsed);
        break;
    default:
        c = C_OFF; fac = 0u;
        break;
    }

    c = scale(c, fac, FAC_FULL);           /* pattern factor      */
    c = scale(c, s->brightness, 255u);     /* master brightness   */
    return c;
}
