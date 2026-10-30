/* espnow_link: ESP-NOW transport for fleet_proto. See espnow_link.h. */
#include "espnow_link.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

#define EVQ_DEPTH 8u

static const uint8_t s_bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static wifi_interface_t s_ifx;
static uint8_t          s_channel;
static espnow_rx_cb_t   s_on_msg;
static QueueHandle_t    s_rxq;

typedef struct { uint8_t mac[6]; fp_msg_t msg; } rx_item_t;

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len <= 0) return;
    rx_item_t it;
    if (!fp_decode(data, (size_t)len, &it.msg)) return;   /* drop foreign/corrupt */
    memcpy(it.mac, info->src_addr, 6);
    if (s_rxq) (void)xQueueSend(s_rxq, &it, 0);           /* drop if full */
}

static void worker(void *arg) {
    (void)arg;
    rx_item_t it;
    for (;;) {
        if (xQueueReceive(s_rxq, &it, portMAX_DELAY) == pdTRUE && s_on_msg)
            s_on_msg(it.mac, &it.msg);
    }
}

static esp_err_t add_peer(const uint8_t mac[6]) {
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = s_channel;
    peer.ifidx = s_ifx;
    peer.encrypt = false;
    if (esp_now_is_peer_exist(mac)) return ESP_OK;
    return esp_now_add_peer(&peer);
}

esp_err_t espnow_link_start(wifi_interface_t ifx, uint8_t channel, espnow_rx_cb_t on_msg) {
    s_ifx = ifx;
    s_channel = channel;
    s_on_msg = on_msg;
    s_rxq = xQueueCreate(EVQ_DEPTH, sizeof(rx_item_t));
    if (!s_rxq) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_now_init();
    if (err == ESP_OK) err = esp_now_register_recv_cb(recv_cb);
    if (err == ESP_OK) err = add_peer(s_bcast);
    if (err != ESP_OK) { vQueueDelete(s_rxq); s_rxq = NULL; return err; }

    if (xTaskCreate(worker, "espnow", 4096, NULL, 6, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t espnow_link_add_peer(const uint8_t mac[6]) { return add_peer(mac); }

esp_err_t espnow_link_send(const uint8_t mac[6], const uint8_t *frame, size_t len) {
    return esp_now_send(mac, frame, len);
}

const uint8_t *espnow_link_broadcast_addr(void) { return s_bcast; }

esp_err_t espnow_link_self_mac(uint8_t out[6]) { return esp_wifi_get_mac(s_ifx, out); }
