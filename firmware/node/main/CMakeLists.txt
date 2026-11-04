idf_component_register(
    SRCS "main.c" "c3_store.c" "c3_link.c" "c3_fleet.c" "c3_web.c"
    INCLUDE_DIRS "."
    REQUIRES link_proto fleet_proto espnow_link status_led web_auth nvs_flash esp_wifi
             esp_netif esp_event esp_http_server esp_driver_uart esp_timer esp_hw_support
    EMBED_FILES "www/index.html")

# admin_hash.h: the key of the degraded web's signed requests, derived from
# CONFIG_C3_ADMIN_PASSWORD (components/web_auth/project_include.cmake); the
# password itself never enters the firmware.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    web_auth_admin_key(C3_ADMIN_PASSWORD c3)
endif()
