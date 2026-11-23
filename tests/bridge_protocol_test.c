// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "home_assistant.h"
#include "local_tcp.h"
#include "tasmota.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { MAX_EXCHANGES = 3 };
static struct {
    char response[5000], received[760];
} exchange[MAX_EXCHANGES];
static size_t cursor, received_length;
static unsigned count, opened, allowed;
static int64_t first_deadline;
static bool allow_write;

int64_t esp_timer_get_time(void) { return 100; }

int local_tcp_open(const char *ip, unsigned port, int64_t deadline)
{
    assert((!strcmp(ip, "192.168.1.80") && port == 80) || (!strcmp(ip, "192.168.1.81") && port == 8123));
    assert(opened < count && deadline > 100 && deadline <= 1200100);
    if (opened == 0) first_deadline = deadline;
    else assert(deadline == first_deadline);
    cursor = received_length = 0;
    ++opened;
    return 99;
}

bool local_tcp_send(int fd, const char *request, size_t length, int64_t deadline)
{
    assert(fd == 99 && length < sizeof(exchange[0].received) - received_length && deadline == first_deadline);
    char *received = exchange[opened - 1].received;
    memcpy(received + received_length, request, length);
    received_length += length;
    received[received_length] = 0;
    return true;
}

int local_tcp_read(int fd, char *bytes, size_t capacity, int64_t deadline)
{
    assert(fd == 99 && deadline == first_deadline);
    const char *response = exchange[opened - 1].response;
    const size_t available = strlen(response) - cursor;
    if (!available) return 0;
    const size_t take = available < 13 ? available : 13;
    assert(capacity >= take);
    memcpy(bytes, response + cursor, take);
    cursor += take;
    return take;
}

static bool permitted(void)
{
    ++allowed;
    return allow_write;
}

static void queue(unsigned status, const char *body)
{
    assert(count < MAX_EXCHANGES);
    char *response = exchange[count++].response;
    if (!status) {
        *response = 0;
    } else {
        const int n = snprintf(response, sizeof(exchange[0].response), "HTTP/1.1 %u Fixture\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n%s", status, strlen(body), body);
        assert(n > 0 && (size_t)n < sizeof(exchange[0].response));
    }
}

static void end(unsigned checks)
{
    assert(opened == count && allowed == checks);
    opened = count = allowed = 0;
    allow_write = true;
    memset(exchange, 0, sizeof(exchange));
}

static void test_tasmota(void)
{
    static const char status[] = "{\"StatusNET\":{\"Mac\":\"02:08:11:aa:bB:cc\"},"
        "\"StatusSTS\":{\"POWER\":\"OFF\"},\"StatusSNS\":{\"ENERGY\":{\"Power\":12.5,\"Total\":1.234}}}";
    static const char two[] = "{\"StatusNET\":{\"Mac\":\"02:08:11:AA:BB:CC\"},"
        "\"StatusSTS\":{\"POWER1\":\"ON\",\"POWER2\":\"OFF\"},\"StatusSNS\":{\"ENERGY\":{\"Power\":12,\"Total\":2}}}";
    shelly_target t = {.ip = "192.168.1.80", .identity = "020811AABBCC", .password = "t& +=%"};
    shelly_sample sample;
    assert(tasmota_valid_identity(t.identity) && !tasmota_valid_identity("02:08:11:AA:BB:CC"));
    queue(200, status);
    assert(tasmota_status(&t, 1, &sample) == SHELLY_OK && !sample.on && sample.power_known && sample.watts == 12.5 &&
        sample.energy_known && sample.total_mwh == 1234000 && !sample.timer_active);
    assert(strstr(exchange[0].received, "/cm?user=admin&password=%74%26%20%2B%3D%25&cmnd=Status%200 "));
    end(0);
    queue(200, two);
    assert(tasmota_status(&t, 1, &sample) == SHELLY_OK && sample.on && !sample.power_known && !sample.energy_known);
    end(0);
    queue(200, two);
    queue(200, "{\"POWER2\":\"ON\"}");
    assert(tasmota_set(&t, 2, true, permitted) == SHELLY_OK);
    assert(strstr(exchange[1].received, "&cmnd=Power2%20On "));
    end(1);
    queue(200, status);
    assert(tasmota_set(&t, 1, false, permitted) == SHELLY_OK);
    end(0);
    queue(200, status);
    allow_write = false;
    assert(tasmota_set(&t, 1, true, permitted) == SHELLY_DEFERRED);
    end(1);
    queue(200, status);
    queue(200, "{\"Command\":\"Unknown\"}");
    assert(tasmota_set(&t, 1, true, permitted) == SHELLY_PROTOCOL);
    end(1);
    queue(200, status);
    queue(0, "");
    assert(tasmota_set(&t, 1, true, permitted) == SHELLY_NETWORK);
    end(1);
    queue(200, "{\"StatusNET\":{\"Mac\":\"02:08:11:AA:BB:CC\"},\"StatusSTS\":{\"POWER\":\"ON\"}}");
    assert(tasmota_status(&t, 1, &sample) == SHELLY_OK && sample.on && !sample.energy_known);
    end(0);
    queue(200, "{\"StatusNET\":{\"Mac\":\"02:08:11:AA:BB:CD\"},\"StatusSTS\":{\"POWER\":\"OFF\"}}");
    assert(tasmota_set(&t, 1, true, permitted) == SHELLY_IDENTITY);
    end(0);
    queue(200, status);
    assert(tasmota_set(&t, 2, true, permitted) == SHELLY_PROTOCOL);
    end(0);
    queue(401, "");
    assert(tasmota_status(&t, 1, &sample) == SHELLY_AUTH);
    end(0);
    queue(200, "{\"WARNING\":\"Need user=<username>&password=<password>\"}");
    assert(tasmota_status(&t, 1, &sample) == SHELLY_AUTH);
    end(0);
    assert(tasmota_set(&t, 0, true, permitted) == SHELLY_IDENTITY);
    end(0);
}

static const ha_endpoint laboratory = {.ip = "192.168.1.81", .port = 8123, .token = "fixture.auth-token"};

static void state(const char *entity, const char *value, const char *attrs)
{
    char body[500];
    const int n = snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"state\":\"%s\",\"attributes\":%s}",
        entity, value, attrs);
    assert(n > 0 && (size_t)n < sizeof(body));
    queue(200, body);
}

static void test_home_assistant(void)
{
    shelly_sample sample;
    const char *entity = "switch.fixture";
    assert(ha_valid_entity(entity, HA_SWITCH) && !ha_valid_entity(entity, HA_SENSOR));
    assert(!ha_valid_entity("switch.a?b", HA_SWITCH) && !ha_valid_token("token\r\nExtra: hello"));
    state(entity, "off", "{}");
    queue(200, "[]");
    assert(ha_switch_set(&laboratory, entity, true, permitted) == SHELLY_OK);
    assert(strstr(exchange[0].received, "GET /api/states/switch.fixture HTTP/1.1"));
    assert(strstr(exchange[1].received, "POST /api/services/switch/turn_on HTTP/1.1") &&
        strstr(exchange[1].received, "Authorization: Bearer fixture.auth-token\r\n") &&
        strstr(exchange[1].received, "\r\n\r\n{\"entity_id\":\"switch.fixture\"}"));
    end(1);
    state(entity, "off", "{}");
    assert(ha_switch_status(&laboratory, entity, &sample) == SHELLY_OK && !sample.on &&
        !sample.energy_known && !sample.power_known);
    end(0);
    state(entity, "on", "{}");
    assert(ha_switch_status(&laboratory, entity, &sample) == SHELLY_OK && sample.on);
    end(0);
    state("switch.another", "on", "{}");
    assert(ha_switch_set(&laboratory, entity, false, permitted) == SHELLY_IDENTITY);
    end(0);
    queue(403, "");
    assert(ha_switch_set(&laboratory, entity, false, permitted) == SHELLY_AUTH);
    end(0);
    state(entity, "unavailable", "{}");
    assert(ha_switch_set(&laboratory, entity, true, permitted) == SHELLY_PROTOCOL);
    end(0);
    state(entity, "on", "{}");
    allow_write = false;
    assert(ha_switch_set(&laboratory, entity, false, permitted) == SHELLY_DEFERRED);
    end(1);

    double value;
    const char *sensor = "sensor.fixture";
    state(sensor, "-1.25", "{\"unit_of_measurement\":\"kW\",\"device_class\":\"power\"}");
    assert(ha_read_sensor(&laboratory, sensor, HA_POWER_W, &value) == SHELLY_OK && value == -1250);
    end(0);
    state(sensor, "0.0125", "{\"unit_of_measurement\":\"MWh\",\"device_class\":\"energy\"}");
    assert(ha_read_sensor(&laboratory, sensor, HA_ENERGY_WH, &value) == SHELLY_OK && value == 12500);
    end(0);
    state(sensor, "68", "{\"unit_of_measurement\":\"\\u00b0F\"}");
    assert(ha_read_sensor(&laboratory, sensor, HA_TEMPERATURE_C, &value) == SHELLY_OK && value == 20);
    end(0);
    state(sensor, "50", "{\"unit_of_measurement\":\"%\",\"device_class\":\"humidity\"}");
    assert(ha_read_sensor(&laboratory, sensor, HA_BATTERY_PERCENT, &value) == SHELLY_PROTOCOL);
    end(0);
    state(sensor, "unknown", "{\"unit_of_measurement\":\"W\"}");
    assert(ha_read_sensor(&laboratory, sensor, HA_POWER_W, &value) == SHELLY_PROTOCOL);
    end(0);
    state(sensor, "1.04", "{\"unit_of_measurement\":\"VA\"}");
    assert(ha_read_sensor(&laboratory, sensor, HA_POWER_W, &value) == SHELLY_PROTOCOL);
    end(0);
    state(sensor, "0x10", "{\"unit_of_measurement\":\"W\"}");
    assert(ha_read_sensor(&laboratory, sensor, HA_POWER_W, &value) == SHELLY_PROTOCOL);
    end(0);
    state(sensor, "0", "{\"unit_of_measurement\":\"W\"}");
    assert(ha_read_sensor(&laboratory, sensor, HA_POWER_W, &value) == SHELLY_OK && value == 0);
    end(0);
}

static void test_ha_controls(void)
{
    static const char number[] = "number.reserve", select[] = "select.preference", button[] = "button.miner";
    static const char number_attrs[] = "{\"min\":0,\"max\":100,\"step\":0.5}";
    static const char select_attrs[] = "{\"options\":[\"Default\",\"Use \\\"surplus\\\"\",\"Eco\"]}";
    static const char restart_class[] = "{\"device_class\":\"restart\"}";
    ha_control_state seen;
    state(number, "25", number_attrs);
    assert(ha_control_read(&laboratory, number, HA_NUMBER, &seen) == SHELLY_OK &&
        seen.value == 25 && seen.min == 0 && seen.max == 100 && seen.step == 0.5);
    end(0);
    state(number, "25", number_attrs);
    assert(ha_number_set(&laboratory, number, 25.1, permitted) == SHELLY_PROTOCOL);
    end(0);
    state(number, "25", number_attrs);
    assert(ha_number_set(&laboratory, number, INFINITY, permitted) == SHELLY_PROTOCOL);
    end(0);
    state(number, "25", number_attrs);
    queue(200, "[]");
    assert(ha_number_set(&laboratory, number, 35.5, permitted) == SHELLY_OK);
    assert(strstr(exchange[1].received, "POST /api/services/number/set_value ") &&
        strstr(exchange[1].received, "\r\n\r\n{\"entity_id\":\"number.reserve\",\"value\":35.5}"));
    end(1);
    state(select, "Default", select_attrs);
    assert(ha_control_read(&laboratory, select, HA_SELECT, &seen) == SHELLY_OK &&
        seen.options == 3 && !strcmp(seen.text, "Default") && !strcmp(seen.option[1], "Use \"surplus\""));
    end(0);
    state(select, "Default", select_attrs);
    assert(ha_select_set(&laboratory, select, "Other", permitted) == SHELLY_PROTOCOL);
    end(0);
    state(select, "Default", select_attrs);
    queue(200, "[]");
    assert(ha_select_set(&laboratory, select, "Use \"surplus\"", permitted) == SHELLY_OK);
    assert(strstr(exchange[1].received, "POST /api/services/select/select_option ") &&
        strstr(exchange[1].received, "\"option\":\"Use \\\"surplus\\\"\"}"));
    end(1);
    state(button, "unknown", "{\"device_class\":\"update\"}");
    assert(ha_button_press(&laboratory, button, permitted) == SHELLY_PROTOCOL);
    end(0);
    state(button, "unknown", restart_class);
    queue(0, "");
    assert(ha_button_press(&laboratory, button, permitted) == SHELLY_NETWORK);
    assert(strstr(exchange[1].received, "POST /api/services/button/press "));
    end(1);
    state(button, "2026-11-23T01:02:03.456+00:00", restart_class);
    assert(ha_control_read(&laboratory, button, HA_BUTTON, &seen) == SHELLY_OK &&
        !strcmp(seen.text, "2026-11-23T01:02:03.456+00:00"));
    end(0);
}

static void test_ha_switch_mapping(void)
{
    const char *relay = "switch.fixture", *power = "sensor.power", *energy = "sensor.energy";
    shelly_sample sample;
    state(relay, "on", "{}");
    state(power, "0.125", "{\"unit_of_measurement\":\"kW\",\"device_class\":\"power\"}");
    state(energy, "2.4", "{\"unit_of_measurement\":\"kWh\",\"state_class\":\"total_increasing\"}");
    assert(ha_mapped_switch_status(&laboratory, relay, power, energy, &sample) == SHELLY_OK && sample.on &&
        sample.power_known && sample.watts == 125 && sample.energy_known && sample.total_mwh == 2400000);
    assert(first_deadline == 1200100);
    end(0);

    state(relay, "off", "{}");
    state(power, "-20", "{\"unit_of_measurement\":\"W\"}");
    state(energy, "4", "{\"unit_of_measurement\":\"Wh\",\"state_class\":\"measurement\"}");
    assert(ha_mapped_switch_status(&laboratory, relay, power, energy, &sample) == SHELLY_OK && !sample.on &&
        !sample.power_known && !sample.energy_known);
    end(0);

    state(relay, "on", "{}");
    queue(404, "");
    state(energy, "1", "{\"unit_of_measurement\":\"Wh\",\"state_class\":\"total\"}");
    assert(ha_mapped_switch_status(&laboratory, relay, power, energy, &sample) == SHELLY_OK && sample.on &&
        !sample.power_known && sample.energy_known && sample.total_mwh == 1000);
    end(0);

    state(relay, "unavailable", "{}");
    assert(ha_mapped_switch_status(&laboratory, relay, power, energy, &sample) == SHELLY_PROTOCOL);
    end(0);
}

int main(void)
{
    end(0);
    test_tasmota();
    test_home_assistant();
    test_ha_controls();
    test_ha_switch_mapping();
    puts("Tasmota HTTP and Home Assistant REST protocol fixtures passed");
}
