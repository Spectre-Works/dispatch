// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "tasmota_mqtt.h"

#include "tasmota.h"
#include <stdio.h>
#include <string.h>

enum { TOPIC_BYTES = 40 };

bool mqtt_tasmota_valid_topic(const char *topic)
{
    const size_t length = strlen(topic);
    return length && length <= 21 && strcmp(topic, "tasmotas") && strcmp(topic, "sonoffs") &&
        strspn(topic, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == length;
}

static void response_topic(char out[TOPIC_BYTES], const char *device, const char *response)
{
    snprintf(out, TOPIC_BYTES, "stat/%s/%s", device, response);
}

static bool command(mqtt_wire *wire, const char *topic, const char *name, const char *value)
{
    char path[TOPIC_BYTES];
    snprintf(path, sizeof(path), "cmnd/%s/%s", topic, name);
    return mqtt_publish(wire, path, value);
}

static shelly_result query(mqtt_wire *wire, const char *topic, const char *kind, const char *response,
                           const json_record **doc)
{
    if (!command(wire, topic, "Status", kind)) return SHELLY_NETWORK;
    const char *payload;
    const shelly_result result = mqtt_receive(wire, response, NULL, &payload);
    if (result != SHELLY_OK) return result;
    *doc = json_parse_shared(payload);
    return *doc ? SHELLY_OK : SHELLY_PROTOCOL;
}

static shelly_result observe(mqtt_wire *wire, const char *mac, const char *topic, bool writing,
                             shelly_sample *sample, char power[TOPIC_BYTES], char power1[TOPIC_BYTES])
{
    char identity[TOPIC_BYTES], state[TOPIC_BYTES], meter[TOPIC_BYTES];
    response_topic(identity, topic, "STATUS5");
    response_topic(state, topic, "STATUS11");
    response_topic(meter, topic, "STATUS10");
    const char *const topics[] = {identity, state, writing ? power : meter, power1};
    shelly_result result = mqtt_subscribe(wire, topics, writing ? 4 : 3);
    if (result != SHELLY_OK) return result;
    const json_record *doc;
    result = query(wire, topic, "5", identity, &doc);
    if (result != SHELLY_OK) return result;
    if (!tasmota_matches(doc, mac)) return SHELLY_IDENTITY;
    result = query(wire, topic, "11", state, &doc);
    if (result != SHELLY_OK) return result;
    const unsigned outputs = json_field(doc, 1, "StatusSTS");
    *sample = (shelly_sample){0};
    if (!tasmota_observed_output(doc, outputs, 1, &sample->on) || !tasmota_only_output(doc, outputs)) return SHELLY_PROTOCOL;
    if (!writing && query(wire, topic, "10", meter, &doc) == SHELLY_OK) tasmota_meter(doc, sample);
    return SHELLY_OK;
}

shelly_result mqtt_tasmota_status(const mqtt_endpoint *endpoint, const char *mac, const char *topic, shelly_sample *sample)
{
    if (!tasmota_valid_identity(mac) || !mqtt_tasmota_valid_topic(topic)) return SHELLY_IDENTITY;
    mqtt_wire wire;
    shelly_result result = mqtt_open(&wire, endpoint);
    if (result == SHELLY_OK) result = observe(&wire, mac, topic, false, sample, NULL, NULL);
    mqtt_close(&wire);
    return result;
}

shelly_result mqtt_tasmota_set(const mqtt_endpoint *endpoint, const char *mac, const char *topic, bool on,
                              bool (*still_allowed)(void))
{
    if (!tasmota_valid_identity(mac) || !mqtt_tasmota_valid_topic(topic)) return SHELLY_IDENTITY;
    char power[TOPIC_BYTES], power1[TOPIC_BYTES];
    response_topic(power, topic, "POWER");
    response_topic(power1, topic, "POWER1");
    mqtt_wire wire;
    shelly_sample sample;
    shelly_result result = mqtt_open(&wire, endpoint);
    if (result == SHELLY_OK) result = observe(&wire, mac, topic, true, &sample, power, power1);
    if (result == SHELLY_OK && sample.on != on) {
        if (still_allowed && !still_allowed()) {
            result = SHELLY_DEFERRED;
        } else if (!command(&wire, topic, "Power1", on ? "ON" : "OFF")) {
            result = SHELLY_NETWORK;
        } else {
            const char *observed;
            result = mqtt_receive(&wire, power, power1, &observed);
            if (result == SHELLY_OK && strcmp(observed, on ? "ON" : "OFF")) result = SHELLY_PROTOCOL;
        }
    }
    mqtt_close(&wire);
    return result;
}
