# status_led: WS2812 status indicator. Pure-C core (status_led.c, host-tested
# in test/test_status_led.c) + ESP glue (status_led_esp.c: a WS2812 driver over
# the RMT bytes encoder and a priority-1 render task). esp_driver_rmt provides
# the RMT TX driver; the no-op path (STATUS_LED_ENABLE=n) still needs esp_timer.
idf_component_register(SRCS "status_led.c" "status_led_esp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_rmt esp_timer)
