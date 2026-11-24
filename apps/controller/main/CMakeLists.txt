# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
idf_component_register(
    SRCS controller.c network.c installation.c dispatch_timezone.c discovery.c web.c timeline.c operations_store.c notifications.c preset_credentials.c profile_store.c ../../../integrations/integration_profile.c ../../../core/dispatch_policy.c ../../../core/dispatch_evidence.c ../../../integrations/json_record.c ../../../integrations/local_tcp.c ../../../integrations/local_http.c
         ../../../integrations/switches/shelly.c ../../../integrations/switches/shelly_http.c ../../../integrations/miners/cgminer.c ../../../integrations/miners/goldshell_mode.c ../../../integrations/mining-services/mining_path.c
         ../../../integrations/home_assistant.c ../../../integrations/switches/tasmota.c ../../../integrations/switches/outlet.c
         ../../../integrations/mqtt_wire.c ../../../integrations/switches/tasmota_mqtt.c
         ../../../integrations/energy/modbus_tcp.c ../../../integrations/energy/victron_gx.c
         ../../../integrations/energy/fronius.c ../../../integrations/energy/native_grid.c ../../../integrations/energy/ecu_r.c ../../../core/dispatch_daylight.c
    INCLUDE_DIRS ../../../integrations/mining-services ../../../integrations/energy ../../../core ../../../integrations ../../../integrations/switches ../../../integrations/miners
    EMBED_TXTFILES ../../web/index.html
    REQUIRES efuse esp_driver_gpio esp_driver_usb_serial_jtag esp_event esp_http_server esp_netif esp_partition esp_security esp_timer esp-tls esp_wifi http_parser lwip mbedtls nvs_flash
)

idf_build_get_property(dispatch_python PYTHON)
set(dispatch_html "${CMAKE_CURRENT_SOURCE_DIR}/../../web/index.html")
set(dispatch_gzip "${CMAKE_CURRENT_BINARY_DIR}/dispatch_index_html.gz")
add_custom_command(OUTPUT "${dispatch_gzip}"
    COMMAND "${dispatch_python}" -c
        "import gzip, pathlib, sys; source, dest = map(pathlib.Path, sys.argv[1:]); dest.write_bytes(gzip.compress(source.read_bytes(), mtime=0))"
        "${dispatch_html}" "${dispatch_gzip}"
    DEPENDS "${dispatch_html}"
    VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${dispatch_gzip}" BINARY)

if(DISPATCH_LINK_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DISPATCH_LINK_TEST)
endif()
