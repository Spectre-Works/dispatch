# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
idf_component_register(
    SRCS controller.c network.c network_update.c installation.c dispatch_timezone.c discovery.c web.c timeline.c operations_store.c notifications.c preset_credentials.c profile_store.c ../../../integrations/integration_profile.c ../../../core/dispatch_policy.c ../../../core/dispatch_evidence.c ../../../integrations/json_record.c ../../../integrations/local_tcp.c ../../../integrations/local_http.c
         ../../../integrations/switches/shelly.c ../../../integrations/switches/shelly_http.c ../../../integrations/miners/cgminer.c ../../../integrations/miners/goldshell_mode.c ../../../integrations/mining-services/mining_path.c
         ../../../integrations/home_assistant.c ../../../integrations/switches/tasmota.c ../../../integrations/switches/outlet.c
         ../../../integrations/mqtt_wire.c ../../../integrations/switches/tasmota_mqtt.c
         ../../../integrations/energy/modbus_tcp.c ../../../integrations/energy/victron_gx.c
         ../../../integrations/energy/fronius.c ../../../integrations/energy/native_grid.c ../../../integrations/energy/ecu_r.c ../../../core/dispatch_daylight.c
    INCLUDE_DIRS ../../../integrations/mining-services ../../../integrations/energy ../../../core ../../../integrations ../../../integrations/switches ../../../integrations/miners
    REQUIRES app_update efuse esp_driver_gpio esp_driver_usb_serial_jtag esp_event esp_http_server esp_netif esp_partition esp_security esp_timer esp-tls esp_wifi http_parser lwip mbedtls nvs_flash
)

idf_build_get_property(dispatch_python PYTHON)
set(dispatch_web "${CMAKE_CURRENT_SOURCE_DIR}/../../web")
set(dispatch_icon "${CMAKE_CURRENT_SOURCE_DIR}/../../../assets/brand/dispatch-icon.svg")
set(dispatch_html "${CMAKE_CURRENT_BINARY_DIR}/index.html")
set(dispatch_gzip "${CMAKE_CURRENT_BINARY_DIR}/dispatch_index_html.gz")
add_custom_command(OUTPUT "${dispatch_html}" "${dispatch_gzip}"
    COMMAND "${dispatch_python}" "${dispatch_web}/build_dashboard.py" "${dispatch_html}" "${dispatch_gzip}"
    DEPENDS "${dispatch_web}/build_dashboard.py" "${dispatch_web}/index.html" "${dispatch_icon}"
    VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${dispatch_html}" TEXT)
target_add_binary_data(${COMPONENT_LIB} "${dispatch_gzip}" BINARY)

if(DISPATCH_LINK_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DISPATCH_LINK_TEST)
endif()

if(DISPATCH_OTA_CA OR DISPATCH_OTA_SIGNER)
    if(NOT EXISTS "${DISPATCH_OTA_CA}" OR NOT EXISTS "${DISPATCH_OTA_SIGNER}")
        message(FATAL_ERROR "Network Update requires a TLS CA PEM and a signing public key PEM")
    endif()
    if(NOT CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE OR NOT PROJECT_VER MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "Network Update requires rollback and a vMAJOR.MINOR.PATCH project version")
    endif()
    if(NOT DISPATCH_OTA_RELEASE_BASE)
        set(DISPATCH_OTA_RELEASE_BASE "https://github.com/Spectre-Works/dispatch/releases")
    endif()
    if(NOT DISPATCH_OTA_RELEASE_BASE MATCHES "^https://[A-Za-z0-9.:-]+(/[A-Za-z0-9._-]+)+$")
        message(FATAL_ERROR "Network Update requires an HTTPS release base with no trailing slash")
    endif()
    target_add_binary_data(${COMPONENT_LIB} "${DISPATCH_OTA_CA}" TEXT RENAME_TO dispatch_ota_ca)
    target_add_binary_data(${COMPONENT_LIB} "${DISPATCH_OTA_SIGNER}" TEXT RENAME_TO dispatch_ota_signer)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DISPATCH_OTA_RELEASE_BASE="${DISPATCH_OTA_RELEASE_BASE}")
    if(DISPATCH_OTA_TRIAL_FAIL)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE DISPATCH_OTA_TRIAL_FAIL)
    endif()
endif()
