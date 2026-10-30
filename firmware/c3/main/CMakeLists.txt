idf_component_register(
    SRCS "main.c" "c3_store.c" "c3_link.c" "c3_fleet.c" "c3_web.c"
    INCLUDE_DIRS "."
    REQUIRES link_proto fleet_proto espnow_link nvs_flash esp_wifi esp_netif
             esp_event esp_http_server esp_driver_uart esp_timer esp_hw_support mbedtls
    EMBED_FILES "www/index.html")
