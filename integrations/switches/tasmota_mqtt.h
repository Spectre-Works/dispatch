// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_TASMOTA_MQTT_H
#define DISPATCH_TASMOTA_MQTT_H
#include "mqtt_wire.h"

bool mqtt_tasmota_valid_topic(const char *topic);
shelly_result mqtt_tasmota_status(const mqtt_endpoint *endpoint, const char *mac, const char *topic, shelly_sample *sample);
shelly_result mqtt_tasmota_set(const mqtt_endpoint *endpoint, const char *mac, const char *topic, bool on,
                              bool (*still_allowed)(void));
#endif
