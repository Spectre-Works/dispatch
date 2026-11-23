// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "mqtt_wire.h"

#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_tcp.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MQTT_PACKET_BYTES = 1536, MQTT_SEND_BYTES = 384, MQTT_TIMEOUT_US = 1300000 };
// One serial user. A receive replaces the previous payload.
static unsigned char packet[MQTT_PACKET_BYTES + 1];

bool mqtt_valid_name(const char *name)
{
    const size_t length = strlen(name);
    if (!length || length >= sizeof(((mqtt_endpoint *)0)->tls_name) ||
        strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != length) return false;
    for (const char *p = name; *p; ) {
        size_t label = strcspn(p, ".");
        if (!label || label > 63 || p[0] == '-' || p[label - 1] == '-') return false;
        p += label;
        if (*p) {
            ++p;
            if (!*p) return false;
        }
    }
    return true;
}

bool mqtt_valid_ca(const char *ca)
{
    const size_t length = strnlen(ca, MQTT_CA_BYTES);
    return length >= 4 && length < MQTT_CA_BYTES && length % 4 != 1 &&
        strspn(ca, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == length;
}

static bool tls_ready(const mqtt_wire *wire, bool write)
{
    const int64_t remaining = wire->deadline - esp_timer_get_time();
    if (remaining <= 0) return false;
    struct timeval limit = {remaining / 1000000, remaining % 1000000};
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(wire->fd, &descriptors);
    return select(wire->fd + 1, write ? NULL : &descriptors, write ? &descriptors : NULL, NULL, &limit) == 1;
}

static int tls_io(mqtt_wire *wire, unsigned char *bytes, size_t size, bool write)
{
    while (esp_timer_get_time() < wire->deadline) {
        const int result = write ? esp_tls_conn_write(wire->tls, bytes, size)
                                 : esp_tls_conn_read(wire->tls, bytes, size);
        if (result >= 0) return result;
        if (result != ESP_TLS_ERR_SSL_WANT_READ && result != ESP_TLS_ERR_SSL_WANT_WRITE) return -1;
        if (!tls_ready(wire, result == ESP_TLS_ERR_SSL_WANT_WRITE)) return -1;
    }
    return -1;
}

static bool send_bytes(mqtt_wire *wire, unsigned char *bytes, size_t size)
{
    if (!wire->tls) return local_tcp_send(wire->fd, (const char *)bytes, size, wire->deadline);
    while (size) {
        const int n = tls_io(wire, bytes, size, true);
        if (n <= 0) return false;
        bytes += n;
        size -= n;
    }
    return true;
}

static bool read_bytes(mqtt_wire *wire, unsigned char *bytes, size_t size)
{
    while (size) {
        const int n = wire->tls ? tls_io(wire, bytes, size, false)
                                : local_tcp_read(wire->fd, (char *)bytes, size, wire->deadline);
        if (n <= 0) return false;
        bytes += n;
        size -= n;
    }
    return true;
}

static bool send_frame(mqtt_wire *wire, unsigned char type, unsigned char *body, size_t size)
{
    if (size > MQTT_SEND_BYTES) return false;
    unsigned char bytes[3 + MQTT_SEND_BYTES] = {type, (size & 127) | (size >= 128 ? 128 : 0), size >> 7};
    const size_t header = size >= 128 ? 3 : 2;
    memcpy(bytes + header, body, size);
    const bool sent = send_bytes(wire, bytes, header + size);
    explicit_bzero(bytes, header + size);
    return sent;
}

static shelly_result receive_frame(mqtt_wire *wire, unsigned char *type, size_t *size)
{
    unsigned char header[2], extra;
    if (!read_bytes(wire, header, sizeof(header))) return SHELLY_NETWORK;
    *type = header[0];
    *size = header[1] & 127;
    if (header[1] & 128) {
        if (!read_bytes(wire, &extra, 1)) return SHELLY_NETWORK;
        if (!extra || extra > 127) return SHELLY_PROTOCOL;
        *size += (size_t)extra * 128;
    }
    if (*size > MQTT_PACKET_BYTES) return SHELLY_PROTOCOL;
    if (!read_bytes(wire, packet, *size)) return SHELLY_NETWORK;
    packet[*size] = 0;
    return SHELLY_OK;
}

static unsigned char *field(unsigned char *where, const char *value)
{
    size_t size = strlen(value);
    *where++ = size >> 8;
    *where++ = size & 255;
    memcpy(where, value, size);
    return where + size;
}

static shelly_result login(mqtt_wire *wire, const mqtt_endpoint *endpoint)
{
    char client[24];
    snprintf(client, sizeof(client), "dispatch%08lx", (unsigned long)esp_random());
    // MQTT 3.1.1 with clean session, username/password and ten-second keepalive.
    unsigned char body[128 + sizeof(endpoint->user)] = {0, 4, 'M', 'Q', 'T', 'T', 4, 0xc2, 0, 10};
    unsigned char *end = field(body + 10, client);
    end = field(end, endpoint->user);
    end = field(end, endpoint->password);
    const bool sent = send_frame(wire, 0x10, body, end - body);
    explicit_bzero(body, sizeof(body));
    if (!sent) return SHELLY_NETWORK;
    unsigned char type;
    size_t size;
    const shelly_result result = receive_frame(wire, &type, &size);
    if (result != SHELLY_OK) return result;
    if (type != 0x20 || size != 2 || packet[0] != 0) return SHELLY_PROTOCOL;
    if (packet[1] == 4 || packet[1] == 5) return SHELLY_AUTH;
    return !packet[1] ? SHELLY_OK : SHELLY_PROTOCOL;
}

shelly_result mqtt_open(mqtt_wire *wire, const mqtt_endpoint *endpoint)
{
    *wire = (mqtt_wire){.fd = -1, .deadline = esp_timer_get_time() + MQTT_TIMEOUT_US};
    const size_t length = strnlen(endpoint->tls_name, sizeof(endpoint->tls_name));
    if (!endpoint->port || !endpoint->user[0] || !endpoint->password[0] ||
        strnlen(endpoint->user, sizeof(endpoint->user)) == sizeof(endpoint->user) ||
        strnlen(endpoint->password, sizeof(endpoint->password)) == sizeof(endpoint->password) ||
        length == sizeof(endpoint->tls_name) || (length && !mqtt_valid_name(endpoint->tls_name)))
        return SHELLY_AUTH;
    if (length) {
        if (!mqtt_valid_ca(endpoint->ca)) return SHELLY_AUTH;
        const size_t encoded = strlen(endpoint->ca), padded = (encoded + 3) / 4 * 4,
            bytes = padded / 4 * 3;
        unsigned char *ca = malloc(bytes + padded);
        if (!ca) return SHELLY_NETWORK;
        unsigned char *input = ca + bytes;
        for (size_t i = 0; i < encoded; ++i) {
            const unsigned char ch = endpoint->ca[i];
            input[i] = ch == '-' ? '+' : ch == '_' ? '/' : ch;
        }
        memset(input + encoded, '=', padded - encoded);
        size_t used;
        if (mbedtls_base64_decode(ca, bytes, &used, input, padded)) {
            free(ca);
            return SHELLY_AUTH;
        }
        const esp_tls_cfg_t config = {
            .common_name = endpoint->tls_name, .cacert_buf = ca, .cacert_bytes = used,
            .timeout_ms = 100, .non_block = true,
        };
        wire->tls = esp_tls_init();
        const int64_t connect_until = esp_timer_get_time() + 700000;
        int status = -1;
        if (wire->tls) {
            do {
                status = esp_tls_conn_new_async(endpoint->ip, strlen(endpoint->ip), endpoint->port, &config, wire->tls);
                if (!status) vTaskDelay(1);
            } while (!status && esp_timer_get_time() < connect_until);
        }
        const bool connected = status == 1 &&
            esp_tls_get_conn_sockfd(wire->tls, &wire->fd) == ESP_OK;
        free(ca);
        if (!connected) return SHELLY_NETWORK;
    } else {
        wire->fd = local_tcp_open(endpoint->ip, endpoint->port, wire->deadline);
        if (wire->fd < 0) return SHELLY_NETWORK;
    }
    return login(wire, endpoint);
}

void mqtt_close(mqtt_wire *wire)
{
    if (wire->tls) esp_tls_conn_destroy(wire->tls);
    else if (wire->fd >= 0) close(wire->fd);
    *wire = (mqtt_wire){.fd = -1};
}

shelly_result mqtt_subscribe(mqtt_wire *wire, const char *const topics[], unsigned count)
{
    if (!count || count > 5) return SHELLY_PROTOCOL;
    unsigned char body[2 + 5 * (2 + 61 + 1)] = {0, 1};
    unsigned char *end = body + 2;
    for (unsigned i = 0; i < count; ++i) {
        if (strlen(topics[i]) > 61) return SHELLY_PROTOCOL;
        end = field(end, topics[i]);
        *end++ = 0; // Receive at QoS 0 within this clean session.
    }
    if (!send_frame(wire, 0x82, body, end - body)) return SHELLY_NETWORK;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned char type;
        size_t size;
        const shelly_result result = receive_frame(wire, &type, &size);
        if (result != SHELLY_OK) return result;
        if ((type & 0xf0) == 0x30) continue; // Ignore unsolicited data before SUBACK.
        if (type != 0x90 || size != 2 + count || packet[0] != 0 || packet[1] != 1) return SHELLY_PROTOCOL;
        for (unsigned j = 2; j < size; ++j) {
            if (packet[j] == 0x80) return SHELLY_AUTH;
            if (packet[j] != 0) return SHELLY_PROTOCOL;
        }
        return SHELLY_OK;
    }
    return SHELLY_PROTOCOL;
}

bool mqtt_publish(mqtt_wire *wire, const char *topic, const char *payload)
{
    if (strlen(topic) > 61 || strlen(payload) > 24) return false;
    unsigned char body[2 + 61 + 24];
    unsigned char *end = field(body, topic);
    size_t size = strlen(payload);
    memcpy(end, payload, size);
    // QoS 0 / retain clear. DISPATCH does not queue or replay these writes.
    return send_frame(wire, 0x30, body, end - body + size);
}

shelly_result mqtt_receive(mqtt_wire *wire, const char *topic, const char *alternate, const char **payload)
{
    const size_t expected = strlen(topic), second = alternate ? strlen(alternate) : 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned char type;
        size_t size;
        const shelly_result result = receive_frame(wire, &type, &size);
        if (result != SHELLY_OK) return result;
        if ((type != 0x30 && type != 0x31) || size < 2) return SHELLY_PROTOCOL;
        const size_t length = (size_t)packet[0] * 256 + packet[1];
        if (!length || length > size - 2) return SHELLY_PROTOCOL;
        if (type == 0x31 || ((length != expected || memcmp(packet + 2, topic, length)) &&
            (!alternate || length != second || memcmp(packet + 2, alternate, length)))) continue;
        *payload = (const char *)packet + 2 + length;
        return memchr(*payload, 0, size - 2 - length) ? SHELLY_PROTOCOL : SHELLY_OK;
    }
    return SHELLY_PROTOCOL;
}
