# LetzTimer: "a drone just passed" on any Raceband channel. The scan task (lt_scan.c) owns the
# sdr_rx engine, feeds its readings of R1-R8 to pass_detector's scan detector and reports each
# pass over the C5<->bridge UART link (link_proto, letztimer_link.c); the console (lt_console.c)
# and the link only post requests to it. The detector has no amplifier and no status LED.
# Public header: letztimer.h (app_main's boot calls).
idf_component_register(
    SRCS "letztimer.c" "lt_scan.c" "lt_bench.c" "lt_console.c" "fw_id.c" "lt_nvs.c" "lt_log.c" "lt_calib.c" "letztimer_link.c"
    INCLUDE_DIRS "."
    LDFRAGMENTS "lt_hot_iram.lf"
    REQUIRES sdr_rx pass_detector link_proto nvs_flash esp_timer
    PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_driver_usb_serial_jtag hal heap esp_app_format esp_system)

# Firmware identity in every log (FIRMWARE line, fw= fields): CI passes the exact head commit;
# local builds ask git at configure time and mark a dirty tree. The ELF SHA-256 printed beside it
# identifies the binary itself. After C5VRX-5's components CMakeLists (Twotoz and the C5VRX
# contributors). Defined for fw_id.c only, so a new commit recompiles nothing else.
if(NOT LETZTIMER_COMMIT)
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                    OUTPUT_VARIABLE LETZTIMER_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE git_rc)
    if(git_rc EQUAL 0 AND LETZTIMER_COMMIT)
        execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                        OUTPUT_VARIABLE git_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(git_dirty)
            set(LETZTIMER_COMMIT "${LETZTIMER_COMMIT}-dirty")
        endif()
    else()
        set(LETZTIMER_COMMIT "unknown")
    endif()
endif()
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/fw_id.c" PROPERTIES
                            COMPILE_DEFINITIONS "LETZTIMER_COMMIT=\"${LETZTIMER_COMMIT}\"")
