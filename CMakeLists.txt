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
