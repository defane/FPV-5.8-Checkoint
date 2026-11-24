# The version of every firmware image (detector, node, master): LETZTIMER_VERSION when CI passes
# it (1.0.<run number> on main, 1.0.0-pr.<number> on a pull request), else the repository's VERSION
# file with "-dev". Include it before project.cmake: ESP-IDF reads PROJECT_VER there. Bump VERSION for a release.
if(DEFINED LETZTIMER_VERSION)
    set(PROJECT_VER "${LETZTIMER_VERSION}")
else()
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" _letztimer_ver LIMIT_COUNT 1)
    set(PROJECT_VER "${_letztimer_ver}-dev")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../VERSION")
