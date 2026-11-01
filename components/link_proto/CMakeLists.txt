# Pure C: host-tested in test/test_link_proto.c. Shared by firmware/detector and
# firmware/node for the UART config/telemetry link.
idf_component_register(SRCS "link_proto.c" INCLUDE_DIRS "include")
