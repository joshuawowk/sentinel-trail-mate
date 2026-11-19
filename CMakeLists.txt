cmake_minimum_required(VERSION 3.16.0)

set(TRAIL_MATE_IDF_TARGET "tab5" CACHE STRING "Active ESP-IDF target")
set_property(CACHE TRAIL_MATE_IDF_TARGET PROPERTY STRINGS
    tab5
    t_display_p4_tft
    t_display_p4_amoled
    t_display_p4_tft_lr2021
    t_display_p4_amoled_lr2021
)

include("${CMAKE_SOURCE_DIR}/builds/esp_idf/target_profiles.cmake")
set(TRAIL_MATE_IDF_TARGET_DIR "${CMAKE_SOURCE_DIR}/builds/esp_idf/targets/${TRAIL_MATE_IDF_TARGET}")
set(SDKCONFIG_DEFAULTS "${TRAILMATE_ESP_IDF_TARGET_DEFAULTS_${TRAIL_MATE_IDF_TARGET}}")
if(NOT EXISTS "${SDKCONFIG_DEFAULTS}")
    message(FATAL_ERROR "Missing sdkconfig.defaults for TRAIL_MATE_IDF_TARGET='${TRAIL_MATE_IDF_TARGET}' at ${TRAIL_MATE_IDF_TARGET_DIR}")
endif()

# Keep generated sdkconfig files inside the selected build directory so
# different IDF targets/build trees do not fight over stale source-root
# sdkconfig.<target> artifacts.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig.${TRAIL_MATE_IDF_TARGET}")

# Sentinel Bootloader (https://github.com/joshuawowk/sentinel-bootloader) profile:
# layers a size-optimised configuration and a one-slot partition table over the
# Tab5 defaults. Use a separate build directory, e.g. build.tab5_sentinel.
option(TRAIL_MATE_SENTINEL "Build the Tab5 image for Sentinel Bootloader" OFF)
if(TRAIL_MATE_SENTINEL)
    if(NOT TRAIL_MATE_IDF_TARGET STREQUAL "tab5")
        message(FATAL_ERROR "TRAIL_MATE_SENTINEL is only supported for TRAIL_MATE_IDF_TARGET=tab5")
    endif()
    list(APPEND SDKCONFIG_DEFAULTS "${TRAIL_MATE_IDF_TARGET_DIR}/sdkconfig.sentinel")
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig.${TRAIL_MATE_IDF_TARGET}_sentinel")
    message(STATUS "trail-mate Sentinel profile enabled")
endif()

# `builds/esp_idf` owns ESP-IDF target defaults after root legacy removal.
# Board choice stays explicit via `TRAIL_MATE_IDF_TARGET`, while board-specific
# runtime logic continues to live under `platform/esp/boards/<board>`.
message(STATUS "trail-mate IDF target: ${TRAIL_MATE_IDF_TARGET}")
message(STATUS "trail-mate SDKCONFIG path: ${SDKCONFIG}")
set(EXTRA_COMPONENT_DIRS
    builds/esp_idf/main
    platform/esp/idf_components
)
if(TRAIL_MATE_SENTINEL)
    # Wi-Fi through the C6's shared esp-hosted firmware (esp_hosted, esp_wifi_remote). Only
    # this build sees these components, so the other builds never link esp_hosted.
    list(APPEND EXTRA_COMPONENT_DIRS platform/esp/idf_components_sentinel)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(TRAIL_MATE_SENTINEL)
    # Visible to builds/esp_idf/main during the component manager's early expansion, which sees
    # build properties but not this cache variable.
    idf_build_set_property(TRAIL_MATE_SENTINEL 1)
    # Its own lock file: the Sentinel build resolves components the shared dependencies.lock
    # of the other builds does not have.
    idf_build_set_property(DEPENDENCIES_LOCK
        "${CMAKE_SOURCE_DIR}/builds/esp_idf/targets/tab5/dependencies.sentinel.lock")
endif()
project(trail-mate)

if(TRAIL_MATE_SENTINEL)
    # The Sentinel profile builds for size (-Os); keep the renderer and the display port,
    # which draw and rotate every frame of the 1280x720 panel, at -O2.
    # (esp_lvgl_port registers an INTERFACE component; its code is in lvgl_port_lib.)
    idf_component_get_property(trailmate_lvgl_lib lvgl__lvgl COMPONENT_LIB)
    foreach(trailmate_fast_lib ${trailmate_lvgl_lib} lvgl_port_lib)
        if(TARGET ${trailmate_fast_lib})
            target_compile_options(${trailmate_fast_lib} PRIVATE -O2)
        endif()
    endforeach()
endif()
