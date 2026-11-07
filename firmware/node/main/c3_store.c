#include "c3_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

#define NVS_NS   "c3"
#define NVS_KEY  "config"

static SemaphoreHandle_t s_lock;
static c3_snapshot_t s_st;      /* guarded by s_lock */

lp_config_t c3_config_default(void)
{
    const lp_config_t c = {
        .alpha_milli = 250, .active_db_x10 = 80, .hysteresis_db_x10 = 30,
        .drop_db_x10 = 30, .min_peak_db_x10 = 100, .refractory_ms = 1500,
        .stale_ms = 1000, .volume_pct = 60, .muted = 0,
    };
    return c;
}

static void load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    lp_config_t c;
    size_t len = sizeof c;
    if (nvs_get_blob(h, NVS_KEY, &c, &len) == ESP_OK && len == sizeof c) {
        s_st.config = c;
        s_st.has_config = true;
    }
    nvs_close(h);
}

static void save_nvs(const lp_config_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, c, sizeof *c) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void c3_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_st.config = c3_config_default();
    load_nvs();
}

bool c3_store_get_config(lp_config_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st.config;
    const bool has = s_st.has_config;
    xSemaphoreGive(s_lock);
    return has;
}

void c3_store_set_config(const lp_config_t *c, bool persist)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.config = *c;
    s_st.has_config = true;
    xSemaphoreGive(s_lock);
    if (persist) save_nvs(c);
}

void c3_store_set_stats(const lp_stats_t *s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.stats = *s;
    s_st.has_stats = true;
    xSemaphoreGive(s_lock);
}

void c3_store_add_event(const lp_event_t *e)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Shift down; keep events[] newest first. */
    for (unsigned i = C3_EVENTS - 1u; i > 0u; i--) s_st.events[i] = s_st.events[i - 1u];
    s_st.events[0] = *e;
    if (s_st.n_events < C3_EVENTS) s_st.n_events++;
    s_st.total_events++;
    xSemaphoreGive(s_lock);
}

void c3_store_snapshot(c3_snapshot_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}
