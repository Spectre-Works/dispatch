// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_OUTLET_H
#define DISPATCH_OUTLET_H

#include "home_assistant.h"
#include "shelly.h"
#include "tasmota_mqtt.h"

typedef enum { OUTLET_INVALID, OUTLET_SHELLY, OUTLET_TASMOTA, OUTLET_HA, OUTLET_MQTT } outlet_kind;

outlet_kind outlet_identity(const char *identity);
bool outlet_same_target(const shelly_target *a, const shelly_target *b);
bool outlet_valid_password(const shelly_target *target);
bool outlet_timed_test(const char *identity);
bool grid_shelly_identity(const char *identity);
shelly_result outlet_status(const shelly_target *target, const ha_endpoint *ha, const mqtt_endpoint *mqtt, shelly_sample *sample);
shelly_result outlet_set(const shelly_target *target, const ha_endpoint *ha, const mqtt_endpoint *mqtt, bool on, bool (*allowed)(void));
shelly_result outlet_timed_on(const shelly_target *target, unsigned seconds, bool (*allowed)(void));
shelly_result grid_shelly_power(const shelly_target *target, int32_t *watts);
#endif
