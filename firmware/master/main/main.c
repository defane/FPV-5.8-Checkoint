/* Checkpoint C6 master (skeleton): UART to the local C5, centralized speaker,
 * Wi-Fi SoftAP + admin web. ESP-NOW fleet hub and race timing are added next. */
#include "c6_hub.h"
#include "c6_link.h"
#include "c6_store.h"
#include "c6_web.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "c6";

static void wifi_ap_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap = {0};
    const char *ssid = CONFIG_C6_AP_SSID;
    const char *pass = CONFIG_C6_AP_PASSWORD;
    strlcpy((char *)ap.ap.ssid, ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    strlcpy((char *)ap.ap.password, pass, sizeof ap.ap.password);
    ap.ap.channel = CONFIG_C6_AP_CHANNEL;     /* ESP-NOW will share this channel */
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP \"%s\" ch=%d, web on http://192.168.4.1/", ssid, CONFIG_C6_AP_CHANNEL);
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    c6_store_init();
    wifi_ap_start();
    c6_hub_start();      /* ESP-NOW + timing + audio; before the link can report a pass */
    c6_link_start();     /* UART to the local C5 (gate 0) */
    c6_web_start();
}
