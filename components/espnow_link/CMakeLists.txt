# ESP-NOW transport for fleet_proto, shared by firmware/master and firmware/node.
# esp_now.h is provided by the esp_wifi component.
idf_component_register(
    SRCS "espnow_link.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi fleet_proto)
