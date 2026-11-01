# status_led: WS2812 status indicator. Pure-C core here; the ESP glue
# (status_led_esp.c, a bit-banged-over-RMT WS2812 driver + task) is added when
# a firmware wires it in. Host-tested in test/test_status_led.c.
idf_component_register(SRCS "status_led.c"
                       INCLUDE_DIRS "include")
