# sdr_rx: the detector's receive engine, ESP32-C5 only. Ported from widscan58 (CapnBry,
# https://github.com/CapnBry/widscan58, a9fff97); its radio layer derives from esp-sdr
# (ESPARGOS, https://github.com/ESPARGOS/esp-sdr, a8eaac1). GPL-3.0-or-later files.
# Pure-C parts host-tested: sdr_psd.c, sdr_level.c (test/test_sdr_dsp.c), sdr_plan.c
# (test/test_sdr_plan.c).
if(NOT IDF_TARGET STREQUAL "esp32c5")
    idf_component_register()      # discovered by the C3/C6 builds (COMPONENTS main), never built
    return()
endif()
idf_component_register(
    SRCS "sdr_rx.c" "sdr_radio_c5.c" "sdr_recal_c5.c" "sdr_psd.c" "sdr_level.c" "sdr_plan.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    LDFRAGMENTS "hot_iram.lf"
    REQUIRES esp_wifi esp_phy esp_timer esp_event
    PRIV_REQUIRES esp_hw_support esp_rom heap soc nvs_flash)
# The hop runs at -O2 whatever the app's optimisation (widscan58 built everything -O2).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
# The RF dump owns SRAM 0x40820000..0x4083FFFF: no static section may reach it.
target_link_options(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/sram_guard.ld")
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=phy_chip_set_chan_ana"   # c5_recalibrate measures at the LO, not the reference channels
    "-Wl,--wrap=ets_delay_us"            # skip the redundant 300 us wait in phy_set_rfpll_freq while tuning
                                         # (esp_rom_delay_us is its alias: every 300 us one while tuning)
    "-Wl,--wrap=phy_wifi_set_tx_gain_new" # skip the TX gain table (~85 us) in channel setup while tuning
    "-Wl,--wrap=phy_set_rx_gain_cal_dc"  # count PHY tracking's recalibrations (cache invalidation)
    "-Wl,--wrap=phy_write_gain_mem")     # record the 84-entry RX gain table for the PLL-only replay
# Private libphy ABI (phy_param offsets, wrapped internals): pinned to IDF 6.0.2's archive.
set(C5_PHY_ARCHIVE "$ENV{IDF_PATH}/components/esp_phy/lib/esp32c5/libphy.a")
file(SHA256 "${C5_PHY_ARCHIVE}" C5_PHY_SHA256)
if(NOT C5_PHY_SHA256 STREQUAL "dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb")
    if(CONFIG_SDR_RX_ALLOW_UNPINNED_PHY)
        message(WARNING "sdr_rx: unverified C5 libphy ${C5_PHY_SHA256}")
    else()
        message(FATAL_ERROR "sdr_rx: C5 libphy ${C5_PHY_SHA256} is not the pinned IDF 6.0.2 one; "
                            "re-verify the private ABI (spec 2.11) or set SDR_RX_ALLOW_UNPINNED_PHY")
    endif()
endif()
# A generated sdkconfig from before this branch keeps options the defaults no longer set.
if(CONFIG_ESP_PHY_ENABLE_CERT_TEST OR CONFIG_COMPILER_OPTIMIZATION_DEBUG)
    message(FATAL_ERROR "sdr_rx: stale sdkconfig (cert-test archive or -Og): run tools/fw.sh "
                        "clean (never delete a *.defaults) and rebuild (spec 3.5)")
endif()
