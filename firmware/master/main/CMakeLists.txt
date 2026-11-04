idf_component_register(
    SRCS "main.c" "c6_store.c" "c6_link.c" "c6_hub.c" "c6_web.c"
    INCLUDE_DIRS "."
    REQUIRES link_proto fleet_proto race_timing espnow_link audio_player status_led
             web_auth nvs_flash esp_wifi esp_netif esp_event esp_http_server
             esp_driver_uart esp_timer
    EMBED_FILES "www/index.html")

# admin_hash.h: the key of the admin web's signed requests, derived from
# CONFIG_C6_ADMIN_PASSWORD (components/web_auth/project_include.cmake); the
# password itself never enters the firmware.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    web_auth_admin_key(C6_ADMIN_PASSWORD c6)
endif()
