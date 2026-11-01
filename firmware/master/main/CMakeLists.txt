idf_component_register(
    SRCS "main.c" "c6_store.c" "c6_link.c" "c6_hub.c" "c6_web.c"
    INCLUDE_DIRS "."
    REQUIRES link_proto fleet_proto race_timing espnow_link audio_player status_led
             nvs_flash esp_wifi esp_netif esp_event esp_http_server
             esp_driver_uart esp_timer mbedtls
    EMBED_FILES "www/index.html")
