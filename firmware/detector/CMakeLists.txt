cmake_minimum_required(VERSION 3.16)
if(DEFINED C5VRX5_VERSION)
    set(PROJECT_VER "${C5VRX5_VERSION}")
else()
    set(PROJECT_VER "5.0.0-dev")
endif()
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
endif()
# Shared components live at the repository root (../../components): pass_detector,
# audio_player and the C5VRX engine are reused by both firmware targets.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(detector-c5)
