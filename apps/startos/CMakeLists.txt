# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
cmake_minimum_required(VERSION 3.18)
project(dispatch_preview LANGUAGES C ASM)

set(DISPATCH_VERSION "development" CACHE STRING "Dashboard release version")
set(DISPATCH_HTTP_PARSER "" CACHE PATH "ESP-IDF components/http_parser directory")
if(NOT EXISTS "${DISPATCH_HTTP_PARSER}/http_parser.c" OR NOT EXISTS "${DISPATCH_HTTP_PARSER}/http_parser.h")
    message(FATAL_ERROR "Set DISPATCH_HTTP_PARSER to the pinned ESP-IDF HTTP parser directory")
endif()
option(DISPATCH_STATIC "Build a static service image executable" OFF)
if(DISPATCH_STATIC)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EVENT REQUIRED IMPORTED_TARGET libevent)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

set(root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(controller "${root}/apps/controller/main")
set(dashboard "${CMAKE_CURRENT_BINARY_DIR}/index.html")
set(web "${root}/apps/web")
set(icon "${root}/assets/brand/dispatch-icon.svg")
configure_file(dashboard.S.in dashboard.S @ONLY)
add_custom_command(OUTPUT "${dashboard}" "${dashboard}.gz"
    COMMAND "${Python3_EXECUTABLE}" "${web}/build_dashboard.py" --startos "${dashboard}" "${dashboard}.gz"
    DEPENDS "${web}/build_dashboard.py" "${web}/index.html" "${icon}" "${root}/assets/brand/dispatch-masthead.svg"
    VERBATIM)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/dashboard.S"
    PROPERTIES OBJECT_DEPENDS "${dashboard};${dashboard}.gz")
set(shared
    "${controller}/dispatch_timezone.c" "${controller}/web.c"
    "${controller}/timeline.c" "${controller}/operations_store.c" "${controller}/notifications.c"
    "${controller}/profile_store.c"
    "${root}/core/dispatch_policy.c" "${root}/core/dispatch_evidence.c" "${root}/core/dispatch_daylight.c"
    "${root}/integrations/integration_profile.c" "${root}/integrations/json_record.c"
    "${root}/integrations/local_tcp.c" "${root}/integrations/local_http.c"
    "${root}/integrations/home_assistant.c" "${root}/integrations/mqtt_wire.c"
    "${root}/integrations/switches/outlet.c" "${root}/integrations/switches/shelly.c"
    "${root}/integrations/switches/shelly_http.c" "${root}/integrations/switches/tasmota.c"
    "${root}/integrations/switches/tasmota_mqtt.c" "${root}/integrations/miners/cgminer.c"
    "${root}/integrations/miners/goldshell_mode.c" "${root}/integrations/mining-services/mining_path.c"
    "${root}/integrations/energy/modbus_tcp.c" "${root}/integrations/energy/victron_gx.c"
    "${root}/integrations/energy/fronius.c" "${root}/integrations/energy/native_grid.c"
    "${root}/integrations/energy/ecu_r.c")
add_library(dispatch-objects OBJECT platform.c storage.c crypto.c network.c http.c preset_credentials.c
    "${CMAKE_CURRENT_BINARY_DIR}/dashboard.S" "${DISPATCH_HTTP_PARSER}/http_parser.c" ${shared})
target_compile_features(dispatch-objects PUBLIC c_std_17)
target_compile_options(dispatch-objects PUBLIC -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(dispatch-objects PUBLIC DISPATCH_PREVIEW_ONLY=1 DISPATCH_VERSION="${DISPATCH_VERSION}")
target_include_directories(dispatch-objects PUBLIC . platform "${DISPATCH_HTTP_PARSER}" "${controller}"
    "${root}/core" "${root}/integrations" "${root}/integrations/energy" "${root}/integrations/miners"
    "${root}/integrations/switches" "${root}/integrations/mining-services")
target_link_libraries(dispatch-objects PUBLIC OpenSSL::SSL OpenSSL::Crypto PkgConfig::EVENT Threads::Threads ZLIB::ZLIB m)
add_executable(dispatch-preview main.c "${controller}/installation.c")
target_link_libraries(dispatch-preview PRIVATE dispatch-objects)
if(DISPATCH_STATIC)
    target_link_options(dispatch-preview PRIVATE -static)
endif()

option(DISPATCH_REPLAY_FIXTURES "Build the host-only worker replay fixtures" OFF)
if(DISPATCH_REPLAY_FIXTURES)
    foreach(target dispatch-startos-replay dispatch-atom-replay)
        add_executable(${target} "${root}/tests/startos_replay.c")
        target_link_libraries(${target} PRIVATE dispatch-objects)
    endforeach()
    target_compile_definitions(dispatch-atom-replay PRIVATE REPLAY_ATOM_WORKER)
endif()
