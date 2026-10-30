# Pure C: host-tested in test/test_fleet_proto.c. Shared by firmware/c6 and
# firmware/c3 for the ESP-NOW fleet link. Reuses link_proto (lp_config_t, CRC).
idf_component_register(SRCS "fleet_proto.c" INCLUDE_DIRS "include" REQUIRES link_proto)
