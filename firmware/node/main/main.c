/* Checkpoint C3 node bridge: UART to the local C5 + ESP-NOW uplink to the C6
 * master. At boot it probes the C6; if reached it runs as a headless relay
 * (forwarding passes and the C5's health, applying the C6's config/channel and
 * commands). If the C6 is not reached within the timeout it falls back to a
 * degraded local service: its own Wi-Fi AP + web for band selection, results,
 * the C5's health and its calibrate/reboot commands. */
#include "c3_fleet.h"
#include "c3_link.h"
#include "c3_store.h"
#include "c3_web.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "status_led.h"
#include <string.h>

static const char *TAG = "c3";

/* Degraded-mode handlers: the local store feeds the local web. */
static void deg_pass(const lp_event_t *e) { status_led_pass(); c3_store_add_event(e); }
static void deg_stats(const lp_stats_t *s) { c3_store_set_stats(s); }

/* AP on the fleet channel (shared with the C6 AP and ESP-NOW). Always up: it
 * carries ESP-NOW in relay mode and the web in degraded mode. */
static void wifi_ap_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap = {0};
    const char *ssid = CONFIG_C3_AP_SSID;
    const char *pass = CONFIG_C3_AP_PASSWORD;
    strlcpy((char *)ap.ap.ssid, ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    strlcpy((char *)ap.ap.password, pass, sizeof ap.ap.password);
    ap.ap.channel = CONFIG_C3_AP_CHANNEL;
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    status_led_start();   /* onboard WS2812 (GPIO10): boot/pairing/healthy/degraded */
    c3_store_init();
    c3_fleet_init();
    wifi_ap_start();
    ESP_ERROR_CHECK(c3_fleet_start_espnow(CONFIG_C3_AP_CHANNEL));

    c3_link_set_handlers(c3_fleet_report_pass, c3_fleet_report_stats);
    c3_link_start();

    if (c3_fleet_try_online(CONFIG_C3_FLEET_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "relay mode: C6 reached (headless)");
    } else {
        ESP_LOGW(TAG, "degraded mode: C6 not reached; local AP + web");
        c3_fleet_set_degraded();
        c3_link_set_handlers(deg_pass, deg_stats);
        c3_web_start();
    }
}
