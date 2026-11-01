#include "c3_fleet.h"
#include "c3_link.h"
#include "espnow_link.h"
#include "esp_log.h"
#include "esp_wifi_types.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "fleet_proto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "c3_fleet";
#define NVS_NS "c3fleet"

static uint8_t   s_c6[6];
static bool      s_paired;
static uint8_t   s_node_id, s_gate, s_channel;
static uint32_t  s_nonce;
static int64_t   s_offset_ms;      /* fleet_ms - local_ms, from beacons */
static volatile bool s_master_seen;
static uint32_t  s_pass_seq;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t fleet_ms(void) { return (uint32_t)((int64_t)now_ms() + s_offset_ms); }

static void save_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "c6mac", s_c6, 6);
    nvs_set_u8(h, "node_id", s_node_id);
    nvs_set_u8(h, "gate", s_gate);
    nvs_set_u8(h, "chan", s_channel);
    nvs_set_u8(h, "paired", s_paired ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void c3_fleet_init(void) {
    s_nonce = esp_random();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t paired = 0;
    size_t len = 6;
    if (nvs_get_u8(h, "paired", &paired) == ESP_OK && paired &&
        nvs_get_blob(h, "c6mac", s_c6, &len) == ESP_OK && len == 6) {
        nvs_get_u8(h, "node_id", &s_node_id);
        nvs_get_u8(h, "gate", &s_gate);
        nvs_get_u8(h, "chan", &s_channel);
        s_paired = true;
    }
    nvs_close(h);
}

static void apply_config(const fp_config_t *c) {
    s_gate = c->gate_index;
    s_channel = c->channel;
    c3_link_push_config(&c->det);       /* detector/audio tunables to the C5 */
    c3_link_set_channel(c->channel);    /* retune the C5 to the assigned channel */
}

static void on_msg(const uint8_t mac[6], const fp_msg_t *msg) {
    switch (msg->type) {
    case FP_MSG_TIME_BEACON:
        s_offset_ms = (int64_t)msg->time_beacon.fleet_ms - (int64_t)now_ms();
        s_master_seen = true;
        break;
    case FP_MSG_PAIR_ACCEPT:
        if (msg->pair_accept.nonce == s_nonce) {
            memcpy(s_c6, mac, 6);
            s_node_id = msg->pair_accept.node_id;
            s_gate = msg->pair_accept.gate_index;
            s_channel = msg->pair_accept.channel;
            s_paired = true;
            espnow_link_add_peer(mac);
            save_nvs();
            s_master_seen = true;
            ESP_LOGI(TAG, "paired: node=%u gate=%u ch=%u", s_node_id, s_gate, s_channel);
        }
        break;
    case FP_MSG_CONFIG:
        apply_config(&msg->config);
        if (s_paired) save_nvs();
        s_master_seen = true;
        break;
    default:
        break;
    }
}

esp_err_t c3_fleet_start_espnow(uint8_t channel) {
    return espnow_link_start(WIFI_IF_AP, channel, on_msg);
}

static void send_pair_request(void) {
    const fp_pair_request_t r = { .nonce = s_nonce, .seen_channel = s_channel, .fw_major = 5 };
    uint8_t buf[FP_MAX_FRAME];
    size_t n = fp_encode_pair_request(buf, sizeof buf, &r);
    if (n) espnow_link_send(espnow_link_broadcast_addr(), buf, n);
}

static void send_heartbeat(void) {
    const fp_heartbeat_t hb = { .role = FP_ROLE_NODE, .uptime_ms = now_ms() };
    uint8_t buf[FP_MAX_FRAME];
    size_t n = fp_encode_heartbeat(buf, sizeof buf, &hb);
    if (n) espnow_link_send(s_c6, buf, n);
}

bool c3_fleet_try_online(uint32_t timeout_ms) {
    if (s_paired) espnow_link_add_peer(s_c6);
    uint32_t waited = 0, last_tx = 0;
    while (waited < timeout_ms) {
        if (now_ms() - last_tx >= 500u || last_tx == 0) {
            last_tx = now_ms();
            if (s_paired) send_heartbeat(); else send_pair_request();
        }
        if (s_master_seen) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return s_master_seen;
}

void c3_fleet_report_pass(const lp_event_t *e) {
    if (!s_paired) return;
    const fp_gate_pass_t gp = { .gate_index = s_gate, .channel = s_channel,
                                .peak_db_x10 = e->peak_db_x10, .fleet_ms = fleet_ms(),
                                .seq = s_pass_seq++ };
    uint8_t buf[FP_MAX_FRAME];
    size_t n = fp_encode_gate_pass(buf, sizeof buf, &gp);
    if (n) espnow_link_send(s_c6, buf, n);
}

void c3_fleet_report_stats(const lp_stats_t *s) {
    if (!s_paired) return;
    const fp_node_status_t ns = { .gate_index = s_gate, .channel = s_channel,
                                  .cn_x10 = s->cn_x10, .state = s->state,
                                  .passes = s->played, .flags = 0 };
    uint8_t buf[FP_MAX_FRAME];
    size_t n = fp_encode_node_status(buf, sizeof buf, &ns);
    if (n) espnow_link_send(s_c6, buf, n);
}
