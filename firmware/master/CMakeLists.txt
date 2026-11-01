cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
endif()
# Shares the repository-root components (link_proto, audio_player, fleet_proto,
# race_timing, espnow_link). That dir also holds the C5-only c5vrx_rx engine
# (C5 BitScrambler); restrict the build to main's dependency closure so it is
# discovered but not built for the C6.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One firmware, two targets: C6 (default) or a spare C5 as a stand-in master
# (-DIDF_TARGET=esp32c5). Name the binary by role + chip so the two images are
# distinguishable (master-c6.bin vs master-c5.bin).
if("$ENV{IDF_TARGET}" STREQUAL "esp32c5" OR "${IDF_TARGET}" STREQUAL "esp32c5")
    project(master-c5)
else()
    project(master-c6)
endif()
