# Checkpoint: "a drone just passed" on any Raceband channel. The scan task (cp_scan.c) owns the
# sdr_rx engine, feeds its readings of R1-R8 to pass_detector's scan detector and reports each
# pass over the C5<->bridge UART link (link_proto, checkpoint_link.c); the console (cp_console.c)
# and the link only post requests to it. The detector has no amplifier and no status LED.
# Public header: checkpoint.h (app_main's boot calls).
idf_component_register(
    SRCS "checkpoint.c" "cp_scan.c" "cp_bench.c" "cp_console.c" "fw_id.c" "cp_nvs.c" "cp_log.c" "checkpoint_link.c"
    INCLUDE_DIRS "."
    LDFRAGMENTS "cp_hot_iram.lf"
    REQUIRES sdr_rx pass_detector link_proto nvs_flash esp_timer
    PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_driver_usb_serial_jtag hal heap esp_app_format esp_system)

# Firmware identity in every log (FIRMWARE line, fw= fields): CI passes the exact head commit;
# local builds ask git at configure time and mark a dirty tree. The ELF SHA-256 printed beside it
# identifies the binary itself. After C5VRX-5's components CMakeLists (Twotoz and the C5VRX
# contributors). Defined for fw_id.c only, so a new commit recompiles nothing else.
if(NOT CHECKPOINT_COMMIT)
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                    OUTPUT_VARIABLE CHECKPOINT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE git_rc)
    if(git_rc EQUAL 0 AND CHECKPOINT_COMMIT)
        execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                        OUTPUT_VARIABLE git_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(git_dirty)
            set(CHECKPOINT_COMMIT "${CHECKPOINT_COMMIT}-dirty")
        endif()
    else()
        set(CHECKPOINT_COMMIT "unknown")
    endif()
endif()
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/fw_id.c" PROPERTIES
                            COMPILE_DEFINITIONS "CHECKPOINT_COMMIT=\"${CHECKPOINT_COMMIT}\"")
