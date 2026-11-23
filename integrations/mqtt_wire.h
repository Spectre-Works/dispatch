// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_MQTT_WIRE_H
#define DISPATCH_MQTT_WIRE_H
#include "shelly.h"
#include <stddef.h>

enum { MQTT_CA_BYTES = 1601 };
typedef struct {
    char ip[16], tls_name[96], user[49], password[65];
    char ca[MQTT_CA_BYTES]; // Unpadded base64url encoding of one DER broker CA certificate.
    uint16_t port;
} mqtt_endpoint;

typedef struct {
    void *tls;
    int fd;
    int64_t deadline;
} mqtt_wire;

bool mqtt_valid_name(const char *name);
bool mqtt_valid_ca(const char *ca);
shelly_result mqtt_open(mqtt_wire *wire, const mqtt_endpoint *endpoint);
void mqtt_close(mqtt_wire *wire);
shelly_result mqtt_subscribe(mqtt_wire *wire, const char *const topics[], unsigned count);
bool mqtt_publish(mqtt_wire *wire, const char *topic, const char *payload);
shelly_result mqtt_receive(mqtt_wire *wire, const char *topic, const char *alternate, const char **payload);
#endif
