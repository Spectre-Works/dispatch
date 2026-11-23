// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_HOME_ASSISTANT_H
#define DISPATCH_HOME_ASSISTANT_H

#include "shelly.h"
#include "energy/ecu_r.h"

enum { HA_ENTITY_BYTES = 81, HA_OPTION_BYTES = 49, HA_OPTIONS = 8 };
typedef struct {
    char ip[16];
    uint16_t port;
    char token[256];
} ha_endpoint;

typedef enum { HA_SWITCH, HA_SENSOR, HA_NUMBER, HA_SELECT, HA_BUTTON } ha_entity_kind;
typedef enum { HA_POWER_W, HA_ENERGY_WH, HA_TEMPERATURE_C, HA_BATTERY_PERCENT } ha_metric;

typedef struct {
    double value, min, max, step;
    char text[HA_OPTION_BYTES];
    char option[HA_OPTIONS][HA_OPTION_BYTES];
    uint8_t options;
} ha_control_state;

bool ha_valid_entity(const char *entity, ha_entity_kind kind);
bool ha_valid_token(const char *token);
ecu_result sdk_solar_read(const ha_endpoint *endpoint, ecu_reading *reading);
shelly_result ha_switch_status(const ha_endpoint *endpoint, const char *entity, shelly_sample *sample);
shelly_result ha_mapped_switch_status(const ha_endpoint *endpoint, const char *entity, const char *power_entity,
                                      const char *energy_entity, shelly_sample *sample);
shelly_result ha_switch_set(const ha_endpoint *endpoint, const char *entity, bool on, bool (*still_allowed)(void));
shelly_result ha_read_sensor(const ha_endpoint *endpoint, const char *entity, ha_metric metric, double *value);
shelly_result ha_control_read(const ha_endpoint *endpoint, const char *entity, ha_entity_kind kind,
                              ha_control_state *state);
shelly_result ha_number_set(const ha_endpoint *endpoint, const char *entity, double value,
                            bool (*still_allowed)(void));
shelly_result ha_select_set(const ha_endpoint *endpoint, const char *entity, const char *option,
                            bool (*still_allowed)(void));
shelly_result ha_button_press(const ha_endpoint *endpoint, const char *entity, bool (*still_allowed)(void));
#endif
