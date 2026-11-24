#!/bin/sh
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0

set -eu

if [ "$#" -gt 1 ]; then
    printf 'Usage: %s [--with-idf]\n' "$0" >&2
    exit 2
fi

case "${1:-}" in
    '') ;;
    --with-idf) ;;
    -h|--help)
        printf 'Usage: %s [--with-idf]\n' "$0"
        printf 'Run repository and host checks. --with-idf adds protocol fixtures using the activated ESP-IDF HTTP parser.\n'
        exit 0
        ;;
    *)
        printf 'Usage: %s [--with-idf]\n' "$0" >&2
        exit 2
        ;;
esac

if [ "${1:-}" = --with-idf ]; then
    if [ -z "${IDF_PATH:-}" ] || [ ! -f "$IDF_PATH/components/http_parser/http_parser.c" ]; then
        printf 'Activate ESP-IDF v6.1.1 before running --with-idf.\n' >&2
        exit 2
    fi
fi

cd "$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
trap 'exit 1' HUP INT TERM

run_test() {
    name=$1
    shift
    printf '\nRunning %s\n' "$name"
    "${CC:-cc}" -Wall -Wextra -Werror "$@" -o "$test_dir/$name"
    "$test_dir/$name"
}

python3 tools/check_repository.py

run_test policy -std=c11 -Icore -Iintegrations \
    tests/dispatch_policy_test.c core/dispatch_policy.c integrations/json_record.c
run_test integration-profile -std=c11 -Iintegrations \
    tests/integration_profile_test.c integrations/integration_profile.c integrations/json_record.c
run_test profile-store -std=gnu11 -Itests/fixtures/profiles -Iapps/controller/main -Iintegrations \
    tests/profile_store_test.c apps/controller/main/profile_store.c integrations/integration_profile.c integrations/json_record.c
run_test controller-storage -std=gnu11 -Icore -Iapps/controller/main -Itests/fixtures/flash -Itests/fixtures/profiles \
    tests/controller_storage_test.c apps/controller/main/timeline.c apps/controller/main/operations_store.c
run_test evidence -std=c11 -Icore \
    tests/dispatch_evidence_test.c core/dispatch_evidence.c
run_test daylight -std=c11 -Icore \
    tests/dispatch_daylight_test.c core/dispatch_daylight.c -lm
run_test timezone -std=c11 -Iapps/controller/main \
    tests/controller_timezone_test.c apps/controller/main/dispatch_timezone.c

printf '#include <stdint.h>\nint64_t esp_timer_get_time(void);\n' > "$test_dir/esp_timer.h"
run_test notification -std=c11 -I"$test_dir" -Iapps/controller/main -Iintegrations \
    tests/notification_test.c apps/controller/main/notifications.c
run_test victron-gx -std=c11 -I"$test_dir" -Iintegrations -Iintegrations/energy -Iintegrations/switches \
    tests/victron_gx_test.c integrations/json_record.c integrations/integration_profile.c integrations/energy/modbus_tcp.c integrations/energy/victron_gx.c
run_test native-grid -std=c11 -I"$test_dir" -Iintegrations -Iintegrations/energy -Iintegrations/switches \
    tests/native_grid_test.c integrations/energy/native_grid.c integrations/energy/modbus_tcp.c -lm
run_test shelly -std=gnu11 -I"$test_dir" -Iintegrations -Iintegrations/switches \
    tests/shelly_test.c integrations/integration_profile.c integrations/switches/shelly.c integrations/json_record.c -lm
run_test fronius -std=c11 -Iintegrations -Iintegrations/energy -Iintegrations/switches \
    tests/fronius_test.c integrations/integration_profile.c integrations/energy/fronius.c integrations/json_record.c -lm
run_test goldshell -std=gnu11 -Itests/fixtures -Iintegrations/miners -Iintegrations \
    tests/goldshell_mode_test.c integrations/miners/goldshell_mode.c integrations/json_record.c

if [ "${1:-}" = --with-idf ]; then
    parser_dir="$IDF_PATH/components/http_parser"
    run_test bridge-protocol -std=gnu11 -I"$test_dir" -I"$parser_dir" -Iintegrations -Iintegrations/switches \
        tests/bridge_protocol_test.c integrations/integration_profile.c integrations/home_assistant.c integrations/switches/tasmota.c \
        integrations/json_record.c integrations/local_http.c "$parser_dir/http_parser.c" -lm
    run_test tasmota-mqtt -std=gnu11 -I"$test_dir" -Itests/fixtures/mqtt -I"$parser_dir" \
        -Iintegrations -Iintegrations/switches tests/tasmota_mqtt_test.c integrations/integration_profile.c integrations/switches/tasmota_mqtt.c \
        integrations/switches/tasmota.c integrations/mqtt_wire.c integrations/json_record.c \
        integrations/local_http.c "$parser_dir/http_parser.c" -lm
    run_test mining-path -std=gnu11 -I"$test_dir" -I"$parser_dir" \
        -Iintegrations -Iintegrations/miners -Iintegrations/mining-services \
        tests/mining_path_test.c integrations/integration_profile.c integrations/mining-services/mining_path.c integrations/miners/cgminer.c \
        integrations/json_record.c integrations/local_http.c "$parser_dir/http_parser.c"
    if [ -n "${DISPATCH_SDK_PATH:-}" ]; then
        mkdir "$test_dir/lwip"
        printf '#include <arpa/inet.h>\n#include <sys/select.h>\n#include <unistd.h>\n' > "$test_dir/lwip/sockets.h"
        "${CC:-cc}" -Wall -Wextra -Werror -std=gnu11 \
            -I"$test_dir" -I"$parser_dir" -Iintegrations -Iintegrations/switches \
            tests/sdk_bridge_test.c integrations/home_assistant.c integrations/json_record.c \
            integrations/local_tcp.c integrations/local_http.c "$parser_dir/http_parser.c" -lm -o "$test_dir/sdk-bridge"
        python3 tests/sdk_bridge_probe.py "$DISPATCH_SDK_PATH" "$test_dir/sdk-bridge"
    fi
fi
