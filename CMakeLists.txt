# =============================================================================
# Root CMakeLists.txt for ESP-IDF project
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# Set project name
set(PROJECT_NAME "electronic_badge")

# Include IDF's cmake build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Create the project
project(${PROJECT_NAME})

# ---------------------------------------------------------------------------
# Pre-build: convert PNG weather icons → RGB565+alpha binary for SPIFFS
#
# - PNG sources:  main/resources/icons/<code>-fill.png
# - SPIFFS target: storage/<code>-fill.bin (fread() directly by display.c)
# - Icon size:     48x48 (fits 51 icons in 512 KB SPIFFS partition)
# - Redundant PNGs are removed from storage/ before each build to avoid
#   accidentally flashing orphan files.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
    message(STATUS "[weather-icons] Converting PNG → RGB565+alpha binaries (48x48)")
    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}" "${PROJECT_DIR}/scripts/png_to_rgb565a.py"
            "${PROJECT_DIR}/main/resources/icons"
            "${PROJECT_DIR}/storage"
        RESULT_VARIABLE _png_conv_result
        OUTPUT_VARIABLE _png_conv_output
        ERROR_VARIABLE _png_conv_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _png_conv_result EQUAL 0)
        message(WARNING "[weather-icons] Conversion failed: ${_png_conv_error}")
        message(WARNING "                  SPIFFS image will use stale .bin files.")
    else()
        message(STATUS "[weather-icons] OK — see output above for size vs SPIFFS limit")
    endif()
else()
    message(WARNING "[weather-icons] Python3 not found — skipping PNG → binary conversion.")
    message(WARNING "                SPIFFS weather icons will not be regenerated.")
endif()

# Auto-generate SPIFFS image from storage/ directory
# and include it in idf.py flash targets
spiffs_create_partition_image(spiffs ${PROJECT_DIR}/storage FLASH_IN_PROJECT)
