// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "home_assistant.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec * INT64_C(1000000) + now.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const ha_endpoint endpoint = {.ip = "127.0.0.1", .port = atoi(argv[1]),
        .token = "fixture-sdk-bridge-token-0000000"};
    shelly_sample sample;
    assert(ha_switch_status(&endpoint, "switch.demo", &sample) == SHELLY_OK && !sample.on);
    assert(ha_switch_set(&endpoint, "switch.demo", true, NULL) == SHELLY_OK);
    assert(ha_mapped_switch_status(&endpoint, "switch.demo", "sensor.power", "sensor.energy",
                                   &sample) == SHELLY_OK);
    assert(sample.on && sample.power_known && sample.watts == 300 &&
           sample.energy_known && sample.total_mwh == 1500000);
    ecu_reading production;
    assert(sdk_solar_read(&endpoint, &production) == ECU_OK);
    assert(!strcmp(production.id, "123456789012") && production.watts == 1300);
    assert(production.today_centi_kwh == 120 && production.lifetime_deci_kwh == 5000 &&
           production.inverters == 2 && production.online == 1);
    assert(sdk_solar_read(&endpoint, &production) == ECU_PROTOCOL);
    double grid;
    assert(ha_read_sensor(&endpoint, "sensor.grid", HA_POWER_W, &grid) == SHELLY_OK && grid == -125);
    assert(ha_switch_set(&endpoint, "switch.demo", false, NULL) == SHELLY_OK);
    assert(ha_switch_status(&endpoint, "switch.demo", &sample) == SHELLY_OK && !sample.on);

    ha_control_state control;
    assert(ha_number_set(&endpoint, "number.limit", 15, NULL) == SHELLY_OK);
    assert(ha_control_read(&endpoint, "number.limit", HA_NUMBER, &control) == SHELLY_OK && control.value == 15);
    assert(ha_select_set(&endpoint, "select.mode", "Low", NULL) == SHELLY_OK);
    assert(ha_control_read(&endpoint, "select.mode", HA_SELECT, &control) == SHELLY_OK && !strcmp(control.text, "Low"));
    assert(ha_button_press(&endpoint, "button.restart", NULL) == SHELLY_OK);
    assert(ha_control_read(&endpoint, "button.restart", HA_BUTTON, &control) == SHELLY_OK &&
           !strcmp(control.text, "2026-11-23T00:00:00+00:00"));
    puts("DISPATCH production REST adapter interoperates with the SDK bridge");
}
