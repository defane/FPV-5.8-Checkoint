cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
endif()
# Shares the repository-root components (link_proto) with firmware/detector. That dir
# also holds C5-only components (c5vrx_rx needs the C5 BitScrambler); restrict
# the build to main and its dependency closure so they are discovered but not
# built for the C3.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(checkpoint_c3)
