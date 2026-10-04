# Spectr Node firmware — product firmware built on the ESPectre Native base.
#
# Build:
#   idf.py set-target esp32s3
#   idf.py build
#
# On the VPS, use infra/spectr/scripts/build-firmware.sh (ESP-IDF container).
cmake_minimum_required(VERSION 3.16)

set(SPECTR_FIRMWARE_ROOT "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "Spectr firmware project root")
set(SPECTR_VENDOR_CPP "${CMAKE_CURRENT_LIST_DIR}/vendor/espectre/src/cpp" CACHE INTERNAL "Vendored ESPectre src/cpp")

# Product firmware version. Override with -DSPECTR_FIRMWARE_VERSION=... or the
# SPECTR_FIRMWARE_VERSION environment variable (the build script sets it).
if(NOT DEFINED SPECTR_FIRMWARE_VERSION OR SPECTR_FIRMWARE_VERSION STREQUAL "")
    if(DEFINED ENV{SPECTR_FIRMWARE_VERSION} AND NOT "$ENV{SPECTR_FIRMWARE_VERSION}" STREQUAL "")
        set(SPECTR_FIRMWARE_VERSION "$ENV{SPECTR_FIRMWARE_VERSION}")
    else()
        set(SPECTR_FIRMWARE_VERSION "0.1.0-dev")
    endif()
endif()
set(ESPECTRE_GIT_VERSION "${SPECTR_FIRMWARE_VERSION}")
set(PROJECT_VER "${SPECTR_FIRMWARE_VERSION}")

include("${SPECTR_VENDOR_CPP}/espectre_git_version.cmake")
include("${SPECTR_VENDOR_CPP}/frontend/espectre_frontend_sources.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    "${SPECTR_VENDOR_CPP}/frontend/native/espectre"
    "${ESPECTRE_SDK_ROOT}/runtime/esp_idf/espectre_config"
)

project(spectr-node)
