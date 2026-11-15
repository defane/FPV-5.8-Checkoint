# ESP32-C5 application entry. The receive engine and the checkpoint feature are components
# (sdr_rx, checkpoint); this is just app_main booting them.
idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
    REQUIRES checkpoint sdr_rx nvs_flash esp_timer esp_driver_usb_serial_jtag)
