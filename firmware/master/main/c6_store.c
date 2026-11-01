#include "c6_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static c6_snapshot_t s_st;

void c6_store_init(void) { s_lock = xSemaphoreCreateMutex(); }

void c6_store_set_local_stats(const lp_stats_t *s) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.local = *s;
    s_st.has_local = true;
    xSemaphoreGive(s_lock);
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
    xSemaphoreGive(s_lock);
}
