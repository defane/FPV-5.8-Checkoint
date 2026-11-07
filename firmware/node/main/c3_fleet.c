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
#include "status_led.h"
#include <string.h>

static const char *TAG = "c3_fleet";
#define NVS_NS "c3fleet"

static uint8_t   s_c6[6];
static bool      s_paired;
static uint8_t   s_node_id, s_gate, s_channel;
static uint32_t  s_nonce;
static int64_t   s_offset_ms;      /* fleet_ms - local_ms, from beacons */
static volatile bool s_master_seen;
static volatile uint32_t s_master_last_ms;  /* last ESP-NOW message from the master */
static volatile bool s_degraded;            /* fell back to local AP + web */
static uint32_t  s_pass_seq;
static uint32_t  s_status_seq;

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
}

static void on_msg(const uint8_t mac[6], const fp_msg_t *msg, int8_t rssi) {
    (void)rssi;                    /* the node shows no RSSI; the master tracks it */
    /* Every master broadcasts beacons: only ours says the fleet is reachable, or an unpaired
     * node stops asking to pair and one paired elsewhere relays to nobody, both in silence. */
    const bool ours = s_paired && memcmp(mac, s_c6, 6) == 0;
    if (ours) s_master_last_ms = now_ms();
    switch (msg->type) {
    case FP_MSG_TIME_BEACON:
        if (!ours) break;
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
            s_master_last_ms = now_ms();
            s_master_seen = true;
            ESP_LOGI(TAG, "paired: node=%u gate=%u ch=%u", s_node_id, s_gate, s_channel);
        }
        break;
    case FP_MSG_CONFIG:
        apply_config(&msg->config);
        if (s_paired) save_nvs();
        s_master_seen = true;
        break;
    /* Calibrate/reboot: only from our master. Not keyed on gate_index: the
     * master addresses us by MAC, and our saved gate can lag its record (a
     * CONFIG sent while we were off is lost), which would drop it silently. */
    case FP_MSG_COMMAND:
        if (s_paired && memcmp(mac, s_c6, 6) == 0) {
            ESP_LOGI(TAG, "master command %u (1=cal_full 2=cal_quick 3=reboot) -> C5",
                     msg->command.cmd);
            c3_link_send_command((lp_cmd_t)msg->command.cmd);
        }
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
        if (s_master_seen) break;
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    /* The boot's log is all a relay node says, so it tells what it found. */
    if (s_master_seen)
        ESP_LOGI(TAG, "master %02x:%02x:%02x:%02x:%02x:%02x, node %u, gate %u", s_c6[0], s_c6[1], s_c6[2],
                 s_c6[3], s_c6[4], s_c6[5], s_node_id, s_gate);
    else if (s_paired)
        ESP_LOGW(TAG, "paired master %02x:%02x:%02x:%02x:%02x:%02x not heard", s_c6[0], s_c6[1], s_c6[2],
                 s_c6[3], s_c6[4], s_c6[5]);
    else
        ESP_LOGW(TAG, "not paired: open pairing on the master, then reboot this node");
    return s_master_seen;
}

void c3_fleet_set_degraded(void) { s_degraded = true; }

/* Status LED base: blue degraded, magenta while (re)pairing, green when the
 * master is alive (a fleet message within 5 s). */
sled_base_t c3_fleet_led_base(uint32_t now) {
    if (s_degraded) return SLED_DEGRADED;
    if (!s_paired)  return SLED_PAIRING;
    if (s_master_last_ms != 0u && (uint32_t)(now - s_master_last_ms) < 5000u)
        return SLED_HEALTHY;
    return SLED_PAIRING;
}

void c3_fleet_report_pass(const lp_event_t *e) {
    status_led_pass();   /* flash on every C5 pass, even before we relay it */
    if (!s_paired) return;
    const fp_gate_pass_t gp = { .gate_index = s_gate, .channel = e->channel,   /* the C5 scans */
                                .peak_db_x10 = e->peak_db_x10, .fleet_ms = fleet_ms() - e->age_ms,
                                .seq = s_pass_seq++ };
    uint8_t buf[FP_MAX_FRAME];
    size_t n = fp_encode_gate_pass(buf, sizeof buf, &gp);
    if (n) espnow_link_send(s_c6, buf, n);
}

void c3_fleet_report_stats(const lp_stats_t *s) {
    if (!s_paired) return;
    fp_node_status_t ns = { .gate_index = s_gate, .channel = s_channel,
                            .cn_x10 = s->cn_x10, .state = s->state,
                            .passes = s->passes, .flags = s->flags, .seq = s_status_seq++,
                            .gain = s->gain, .uptime_s = s->uptime_s,
                            .heap_min = s->heap_min, .faults = s->faults };
    memcpy(ns.levels, s->levels, sizeof ns.levels);
    strlcpy(ns.fw, s->fw, sizeof ns.fw);
    uint8_t buf[FP_MAX_FRAME];
    size_t n = fp_encode_node_status(buf, sizeof buf, &ns);
    if (n) espnow_link_send(s_c6, buf, n);
}
