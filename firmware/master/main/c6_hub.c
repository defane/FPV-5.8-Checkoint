#include "c6_hub.h"
#include "c6_store.h"
#include "audio_player.h"
#include "espnow_link.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fleet_proto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link_proto.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "status_led.h"
#include <string.h>

static const char *TAG = "c6_hub";

#define NVS_NS        "c6fleet"
#define BEACON_MS     2000u
#define NODE_LIVE_MS  8000u     /* no traffic this long -> offline */

static SemaphoreHandle_t s_lock;
static race_timing_t     s_rt;
static c6_node_t         s_nodes[C6_MAX_NODES];
static uint8_t           s_num_nodes;
static bool              s_pairing = true;    /* open at boot for bring-up */
static uint32_t          s_beacon_seq;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void audio_start(void) {
    const audio_player_config_t cfg = {
        .bclk_gpio = CONFIG_C6_AUDIO_BCLK_GPIO, .ws_gpio = CONFIG_C6_AUDIO_WS_GPIO,
        .dout_gpio = CONFIG_C6_AUDIO_DOUT_GPIO, .sd_gpio = CONFIG_C6_AUDIO_SD_GPIO,
        .task_priority = 5,
    };
    const esp_err_t err = audio_player_start(&cfg);
    if (err != ESP_OK) ESP_LOGW(TAG, "audio off: %s (no coin.pcm / build)", esp_err_to_name(err));
}

/* Centralized audio volume, persisted in NVS so the web setting survives a reboot. */
static void volume_apply_saved(void) {
    uint8_t v = AUDIO_VOLUME_DEFAULT;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "volume", &v); nvs_close(h); }
    audio_player_set_volume(v);
}

void c6_hub_set_volume(uint8_t pct) {
    if (pct > 100u) pct = 100u;
    audio_player_set_volume(pct);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "volume", pct); nvs_commit(h); nvs_close(h);
    }
}

static lp_config_t default_config(void) {
    const lp_config_t c = {
        .alpha_milli = 300, .active_db_x10 = 150, .hysteresis_db_x10 = 30,
        .drop_db_x10 = 30, .min_peak_db_x10 = 250, .refractory_ms = 1500,
        .stale_ms = 1000, .volume_pct = 60, .muted = 0,
    };
    return c;
}

/* ---- NVS node table ---- */
typedef struct __attribute__((packed)) {
    uint8_t mac[6], node_id, gate_index, channel;
} node_rec_t;

static void nodes_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    node_rec_t rec[C6_MAX_NODES];
    for (unsigned i = 0; i < s_num_nodes; i++) {
        memcpy(rec[i].mac, s_nodes[i].mac, 6);
        rec[i].node_id = s_nodes[i].node_id;
        rec[i].gate_index = s_nodes[i].gate_index;
        rec[i].channel = s_nodes[i].channel;
    }
    nvs_set_u8(h, "count", s_num_nodes);
    if (s_num_nodes) nvs_set_blob(h, "nodes", rec, s_num_nodes * sizeof(node_rec_t));
    nvs_commit(h);
    nvs_close(h);
}

static void nodes_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) == ESP_OK && count && count <= C6_MAX_NODES) {
        node_rec_t rec[C6_MAX_NODES];
        size_t len = count * sizeof(node_rec_t);
        if (nvs_get_blob(h, "nodes", rec, &len) == ESP_OK && len == count * sizeof(node_rec_t)) {
            s_num_nodes = count;
            for (unsigned i = 0; i < count; i++) {
                memcpy(s_nodes[i].mac, rec[i].mac, 6);
                s_nodes[i].node_id = rec[i].node_id;
                s_nodes[i].gate_index = rec[i].gate_index;
                s_nodes[i].channel = rec[i].channel;
                s_nodes[i].cn_x10 = LP_CN_UNKNOWN;
                s_nodes[i].online = false;
            }
        }
    }
    nvs_close(h);
}

static c6_node_t *find_by_mac(const uint8_t mac[6]) {
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (memcmp(s_nodes[i].mac, mac, 6) == 0) return &s_nodes[i];
    return NULL;
}

/* Gate count = highest assigned gate + 1 (gate 0 is always the local C5), so
 * admin-assigned orders with gaps still size race_timing correctly. */
static uint8_t num_gates(void) {
    uint8_t g = 1u;
    for (unsigned i = 0; i < s_num_nodes; i++)
        if ((uint8_t)(s_nodes[i].gate_index + 1u) > g) g = (uint8_t)(s_nodes[i].gate_index + 1u);
    if (g > RT_MAX_GATES) g = RT_MAX_GATES;
    return g;
}

static void send_config_to(const c6_node_t *n) {
    const fp_config_t cfg = { .gate_index = n->gate_index, .channel = n->channel,
                              .enable = 1, .det = default_config() };
    uint8_t buf[FP_MAX_FRAME];
    size_t m = fp_encode_config(buf, sizeof buf, &cfg);
    if (m) espnow_link_send(n->mac, buf, m);
}

static void send_pair_accept(const c6_node_t *n, uint32_t nonce) {
    const fp_pair_accept_t pa = { .nonce = nonce, .node_id = n->node_id,
                                  .gate_index = n->gate_index, .channel = n->channel };
    uint8_t buf[FP_MAX_FRAME];
    size_t m = fp_encode_pair_accept(buf, sizeof buf, &pa);
    if (m) espnow_link_send(n->mac, buf, m);
}

static void handle_pair(const uint8_t mac[6], const fp_pair_request_t *req) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c6_node_t *n = find_by_mac(mac);
    if (!n) {
        if (!s_pairing || s_num_nodes >= C6_MAX_NODES) { xSemaphoreGive(s_lock); return; }
        n = &s_nodes[s_num_nodes];
        memcpy(n->mac, mac, 6);
        n->node_id = s_num_nodes;
        n->gate_index = num_gates();                   /* next free gate (gate 0 is the local C5) */
        n->channel = req->seen_channel;
        n->cn_x10 = LP_CN_UNKNOWN;
        n->online = true;
        n->last_seen_ms = now_ms();
        s_num_nodes++;
        espnow_link_add_peer(mac);
        nodes_save();
        rt_init(&s_rt, &(rt_config_t){ .num_gates = num_gates() });
        ESP_LOGI(TAG, "paired node id=%u gate=%u ch=%u", n->node_id, n->gate_index, n->channel);
    }
    const c6_node_t snap = *n;
    xSemaphoreGive(s_lock);
    send_pair_accept(&snap, req->nonce);
    send_config_to(&snap);
}

static void record_pass(uint8_t gate, uint8_t channel, int32_t peak_db_x10, uint32_t fleet_ms) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    rt_cross(&s_rt, gate, channel, fleet_ms, NULL);
    xSemaphoreGive(s_lock);
    const c6_pass_t p = { .channel = channel, .peak_db_x10 = peak_db_x10,
                          .at_ms = fleet_ms, .gate = gate };
    c6_store_add_pass(&p);
    audio_player_play(AUDIO_SOUND_COIN, audio_pitch_for_channel(channel));
    status_led_pass();   /* white flash on any gate's pass */
}

static void on_msg(const uint8_t mac[6], const fp_msg_t *msg) {
    switch (msg->type) {
    case FP_MSG_PAIR_REQUEST:
        handle_pair(mac, &msg->pair_request);
        break;
    case FP_MSG_GATE_PASS: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c6_node_t *n = find_by_mac(mac);
        uint8_t gate = n ? n->gate_index : msg->gate_pass.gate_index;
        if (n) { n->last_seen_ms = now_ms(); n->online = true; }
        xSemaphoreGive(s_lock);
        record_pass(gate, msg->gate_pass.channel, msg->gate_pass.peak_db_x10, msg->gate_pass.fleet_ms);
        break;
    }
    case FP_MSG_NODE_STATUS: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c6_node_t *n = find_by_mac(mac);
        if (n) { n->cn_x10 = msg->node_status.cn_x10; n->state = msg->node_status.state;
                 n->last_seen_ms = now_ms(); n->online = true; }
        xSemaphoreGive(s_lock);
        break;
    }
    case FP_MSG_HEARTBEAT: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c6_node_t *n = find_by_mac(mac);
        if (n) { n->last_seen_ms = now_ms(); n->online = true; }
        xSemaphoreGive(s_lock);
        break;
    }
    default:
        break;
    }
}

static void beacon_task(void *arg) {
    (void)arg;
    for (;;) {
        const fp_time_beacon_t b = { .seq = s_beacon_seq++, .fleet_ms = now_ms() };
        uint8_t buf[FP_MAX_FRAME];
        size_t m = fp_encode_time_beacon(buf, sizeof buf, &b);
        if (m) espnow_link_send(espnow_link_broadcast_addr(), buf, m);

        const uint32_t t = now_ms();
        bool offline = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (unsigned i = 0; i < s_num_nodes; i++)
            if (s_nodes[i].online && t - s_nodes[i].last_seen_ms > NODE_LIVE_MS)
                s_nodes[i].online = false;
        for (unsigned i = 0; i < s_num_nodes; i++)
            if (!s_nodes[i].online) offline = true;
        const uint8_t num = s_num_nodes;
        const bool pairing = s_pairing;
        xSemaphoreGive(s_lock);

        /* Status LED: magenta while pairing with no gates yet, blue if a paired
         * gate dropped offline, green otherwise. */
        status_led_set_base(num == 0u ? (pairing ? SLED_PAIRING : SLED_HEALTHY)
                                      : (offline ? SLED_DEGRADED : SLED_HEALTHY));

        vTaskDelay(pdMS_TO_TICKS(BEACON_MS));
    }
}

void c6_hub_start(void) {
    s_lock = xSemaphoreCreateMutex();
    audio_start();
    volume_apply_saved();
#if CONFIG_C6_BOOT_CHIME
    audio_player_play(AUDIO_SOUND_COIN, audio_pitch_for_channel(0));  /* speaker check */
#endif
    nodes_load();
    rt_init(&s_rt, &(rt_config_t){ .num_gates = num_gates() });

    esp_err_t err = espnow_link_start(WIFI_IF_AP, CONFIG_C6_AP_CHANNEL, on_msg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "espnow start: %s", esp_err_to_name(err)); return; }
    for (unsigned i = 0; i < s_num_nodes; i++) espnow_link_add_peer(s_nodes[i].mac);

    xTaskCreate(beacon_task, "c6_beacon", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "fleet hub: %u node(s), %u gate(s), pairing %s",
             s_num_nodes, num_gates(), s_pairing ? "open" : "closed");
}

void c6_hub_local_pass(uint8_t channel, int32_t peak_db_x10, uint32_t at_ms) {
    record_pass(0, channel, peak_db_x10, at_ms);   /* gate 0 = master's local C5 */
}

void c6_hub_set_pairing(bool open) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pairing = open;
    xSemaphoreGive(s_lock);
}

bool c6_hub_set_node(uint8_t node_id, uint8_t gate_index, uint8_t channel) {
    if (gate_index < 1u || gate_index >= RT_MAX_GATES) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c6_node_t *n = NULL;
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (s_nodes[i].node_id == node_id) { n = &s_nodes[i]; break; }
    if (!n) { xSemaphoreGive(s_lock); return false; }
    n->gate_index = gate_index;
    n->channel = channel;
    nodes_save();
    rt_init(&s_rt, &(rt_config_t){ .num_gates = num_gates() });
    const c6_node_t snap = *n;
    xSemaphoreGive(s_lock);
    send_config_to(&snap);
    return true;
}

bool c6_hub_remove_node(uint8_t node_id) {
    uint8_t mac[6];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (s_nodes[i].node_id == node_id) { idx = (int)i; break; }
    if (idx < 0) { xSemaphoreGive(s_lock); return false; }
    memcpy(mac, s_nodes[idx].mac, 6);
    for (unsigned i = (unsigned)idx; i + 1u < s_num_nodes; i++) s_nodes[i] = s_nodes[i + 1u];
    s_num_nodes--;
    nodes_save();
    rt_init(&s_rt, &(rt_config_t){ .num_gates = num_gates() });
    xSemaphoreGive(s_lock);
    espnow_link_del_peer(mac);
    return true;
}

void c6_hub_reset_race(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    rt_reset(&s_rt);
    xSemaphoreGive(s_lock);
}

void c6_hub_snapshot(c6_fleet_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->pairing = s_pairing;
    out->num_gates = num_gates();
    out->num_nodes = s_num_nodes;
    out->volume = (uint8_t)audio_player_volume();
    out->uptime_ms = now_ms();
    out->free_heap = esp_get_free_heap_size();
    for (unsigned i = 0; i < s_num_nodes; i++) out->nodes[i] = s_nodes[i];
    out->num_standings = s_rt.num_channels;
    for (unsigned i = 0; i < s_rt.num_channels; i++) {
        out->standings[i].channel = s_rt.ch[i].channel;
        out->standings[i].laps = s_rt.ch[i].laps;
        out->standings[i].last_lap_ms = s_rt.ch[i].last_lap_ms;
        out->standings[i].best_lap_ms = s_rt.ch[i].best_lap_ms;
        out->standings[i].last_gate = s_rt.ch[i].last_gate;
    }
    xSemaphoreGive(s_lock);
}
