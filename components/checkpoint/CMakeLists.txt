# Checkpoint: "a drone just passed" on any Raceband channel. Sweeps R1-R8 through
# c5vrx_rx's public headers and reports each pass over the C5<->C3 UART link (link_proto,
# checkpoint_link.c); the detector has no amplifier and no status LED. Its
# symbols are the strong definitions that override c5vrx_rx's weak checkpoint
# seam. The public contract header (checkpoint.h) lives in c5vrx_rx.
idf_component_register(
    SRCS "checkpoint.c" "checkpoint_link.c" "hop_bench.c" "scan_hop.c"
    INCLUDE_DIRS "."
    REQUIRES c5vrx_rx pass_detector link_proto esp_driver_gpio esp_driver_uart esp_timer)
