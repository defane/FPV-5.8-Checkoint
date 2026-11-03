/* espnow_link: ESP-NOW transport for the fleet, shared by firmware/master and
 * firmware/node. It carries fleet_proto datagrams on a fixed Wi-Fi channel (the
 * channel the C6 SoftAP uses). Wi-Fi must already be initialised and started by
 * the app (AP on the C6, STA on a relaying C3) before espnow_link_start.
 *
 * Received frames are decoded with fp_decode off the Wi-Fi callback and
 * dispatched to on_msg from a worker task, so the callback stays short. */
#pragma once
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "fleet_proto.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from a worker task for every valid received frame. mac is the sender;
 * rssi is the frame's ESP-NOW receive strength in dBm (0 if unavailable). */
typedef void (*espnow_rx_cb_t)(const uint8_t mac[6], const fp_msg_t *msg, int8_t rssi);

/* ifx = WIFI_IF_AP (C6) or WIFI_IF_STA (C3). channel must match the C6 AP. */
esp_err_t espnow_link_start(wifi_interface_t ifx, uint8_t channel, espnow_rx_cb_t on_msg);

esp_err_t espnow_link_add_peer(const uint8_t mac[6]);   /* unicast peer */
esp_err_t espnow_link_del_peer(const uint8_t mac[6]);
esp_err_t espnow_link_send(const uint8_t mac[6], const uint8_t *frame, size_t len);

const uint8_t *espnow_link_broadcast_addr(void);        /* FF:FF:FF:FF:FF:FF */
esp_err_t espnow_link_self_mac(uint8_t out[6]);

#ifdef __cplusplus
}
#endif
