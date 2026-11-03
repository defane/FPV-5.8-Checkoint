#include "c6_store.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static c6_snapshot_t s_st;      /* has_local here = seen once; readers get local_alive() */
static uint32_t s_local_ms;     /* when the last local STATS arrived */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Under s_lock. A C5 that stopped reporting (unplugged, hung, rebooting) is
 * absent: its last C/N and health are stale and nobody hears a command. */
static bool local_alive(void) { return s_st.has_local && now_ms() - s_local_ms <= C6_LOCAL_LIVE_MS; }

void c6_store_init(void) { s_lock = xSemaphoreCreateMutex(); }

void c6_store_set_local_stats(const lp_stats_t *s) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.local = *s;
    s_st.has_local = true;
    s_local_ms = now_ms();
    xSemaphoreGive(s_lock);
}

void c6_store_set_local_config(const lp_config_t *c) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.local_cfg = *c;
    s_st.has_local_cfg = true;
    xSemaphoreGive(s_lock);
}

bool c6_store_has_local(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool has = local_alive();
    xSemaphoreGive(s_lock);
    return has;
}

void c6_store_add_pass(const c6_pass_t *p) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = C6_EVENTS - 1u; i > 0u; i--) s_st.passes[i] = s_st.passes[i - 1u];
    s_st.passes[0] = *p;
    if (s_st.n_passes < C6_EVENTS) s_st.n_passes++;
    s_st.total_passes++;
    xSemaphoreGive(s_lock);
}

void c6_store_snapshot(c6_snapshot_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    out->has_local = local_alive();
    xSemaphoreGive(s_lock);
}
