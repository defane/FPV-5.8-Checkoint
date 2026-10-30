# Checkpoint: "a drone just passed" on the tuned channel. Reads the C5VRX
# receive engine (C/N, channel, state) through c5vrx_rx's public headers and
# rings a coin sound via audio_player when pass_detector fires. Its symbols
# are the strong definitions that override c5vrx_rx's weak checkpoint seam.
# The public contract header (checkpoint.h) lives in c5vrx_rx.
idf_component_register(
    SRCS "checkpoint.c"
    REQUIRES c5vrx_rx pass_detector audio_player esp_timer)
