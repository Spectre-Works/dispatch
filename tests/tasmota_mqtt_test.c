// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "tasmota_mqtt.h"
#include "integration_profile.h"
#include "local_tcp.h"
#include "esp_tls.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char incoming[1200];
static size_t next_byte, incoming_size;
static unsigned connects, sends, controls, allowed;
static int64_t now = 100;
static bool allow_write, tls_fails;

int64_t esp_timer_get_time(void) { return now; }
uint32_t esp_random(void) { return 0x11223344; }
void vTaskDelay(unsigned ticks) { now += 1000 * ticks; }

int mbedtls_base64_decode(unsigned char *dst, size_t max, size_t *used, const unsigned char *src, size_t chars)
{
    assert(chars == 8 && !memcmp(src, "dGVzdCE=", chars) && max >= 5);
    memcpy(dst, "test!", 5);
    *used = 5;
    return 0;
}

esp_tls_t *esp_tls_init(void) { return (esp_tls_t *)1; }
int esp_tls_conn_destroy(esp_tls_t *tls) { assert(tls == (esp_tls_t *)1); return 0; }
int esp_tls_get_conn_sockfd(esp_tls_t *tls, int *fd) { assert(tls == (esp_tls_t *)1); *fd = 99; return 0; }
int esp_tls_conn_new_async(const char *ip, int length, int port, const esp_tls_cfg_t *config, esp_tls_t *tls)
{
    assert(tls == (esp_tls_t *)1 && !strcmp(ip, "192.168.10.40") && length == 13 && port == 8883);
    assert(!strcmp(config->common_name, "broker.example") && !config->skip_common_name &&
           config->non_block && config->timeout_ms == 100 &&
           config->cacert_bytes == 5 && !memcmp(config->cacert_buf, "test!", 5));
    if (tls_fails) return -1;
    ++connects;
    return 1;
}

static int64_t shared_deadline(void) { return 1300100; }

int local_tcp_open(const char *ip, unsigned port, int64_t deadline)
{
    assert(port == 1883 && !strcmp(ip, "192.168.10.40") && deadline == shared_deadline());
    ++connects;
    return 99;
}

static bool contains(const char *bytes, size_t size, const char *part)
{
    const size_t found = strlen(part);
    for (size_t i = 0; i + found <= size; ++i) if (!memcmp(bytes + i, part, found)) return true;
    return false;
}

bool local_tcp_send(int fd, const char *bytes, size_t size, int64_t deadline)
{
    assert(fd == 99 && deadline == shared_deadline() && size > 3);
    const unsigned char type = bytes[0], length = bytes[1];
    assert(length < 128 && length + 2u == size);
    ++sends;
    if (type == 0x10) {
        assert(sends == 1 && !memcmp(bytes + 2, "\x00\x04MQTT\x04\xc2\x00\x0a", 10));
        assert(contains(bytes, size, "dispatch11223344") && contains(bytes, size, "fixtureuser") &&
               contains(bytes, size, "fixturepass"));
    } else if (type == 0x82) {
        assert(sends == 2 && contains(bytes, size, "stat/fixture/STATUS5") && contains(bytes, size, "stat/fixture/STATUS11"));
    } else {
        assert(type == 0x30); // No retain, QoS, or duplicate flag, including commands.
        if (contains(bytes, size, "cmnd/fixture/Power1")) {
            assert(allow_write && allowed == 1 && sends == 5 && !memcmp(bytes + size - 2, "ON", 2));
            ++controls;
        } else {
            assert(contains(bytes, size, "cmnd/fixture/Status"));
            const char *suffix = sends == 3 ? "5" : sends == 4 ? "11" : "10";
            assert(!memcmp(bytes + size - strlen(suffix), suffix, strlen(suffix)));
        }
    }
    return true;
}

int local_tcp_read(int fd, char *bytes, size_t size, int64_t deadline)
{
    assert(fd == 99 && deadline == shared_deadline());
    if (next_byte == incoming_size) {
        now = deadline;
        return -1;
    }
    assert(size > 0);
    *bytes = incoming[next_byte++]; // Fragment fixed headers and bodies.
    return 1;
}

ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *bytes, size_t size)
{
    assert(tls == (esp_tls_t *)1);
    return local_tcp_send(99, bytes, size, shared_deadline()) ? (ssize_t)size : -1;
}

ssize_t esp_tls_conn_read(esp_tls_t *tls, void *bytes, size_t size)
{
    assert(tls == (esp_tls_t *)1);
    return local_tcp_read(99, bytes, size, shared_deadline());
}

static void append(const unsigned char *packet, size_t length)
{
    assert(incoming_size + length <= sizeof(incoming));
    memcpy(incoming + incoming_size, packet, length);
    incoming_size += length;
}

static void publish(unsigned flags, const char *topic, const char *data)
{
    const size_t topic_size = strlen(topic), data_size = strlen(data);
    const size_t length = 2 + topic_size + data_size;
    const unsigned char header[] = {0x30 | flags, length, 0, topic_size};
    assert(length < 128);
    append(header, sizeof(header));
    append((const unsigned char *)topic, topic_size);
    append((const unsigned char *)data, data_size);
}

static void connected(unsigned grants, unsigned code)
{
    const unsigned char connack[] = {0x20, 2, 0, code};
    const unsigned char suback[] = {0x90, 2 + grants, 0, 1, 0, 0, 0, 0};
    append(connack, sizeof(connack));
    if (!code) append(suback, 4 + grants);
}

static void state(const char *mac, const char *relay)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"StatusNET\":{\"Mac\":\"%s\"}}", mac);
    publish(0, "stat/fixture/STATUS5", body);
    if (relay) publish(0, "stat/fixture/STATUS11", relay);
}

static bool permitted(void) { ++allowed; return allow_write; }

static void reset(unsigned sockets, unsigned packets, unsigned writes, unsigned checks)
{
    assert(connects == sockets && sends == packets && controls == writes && allowed == checks);
    incoming_size = next_byte = connects = sends = controls = allowed = 0;
    now = 100;
    allow_write = true;
}

int main(void)
{
    mqtt_endpoint broker = {.ip = "192.168.10.40", .port = 1883,
        .user = "fixtureuser", .password = "fixturepass"};
    const char *mac = "020809AABBCC", *topic = "fixture";
    const char *off = "{\"StatusSTS\":{\"POWER\":\"OFF\"}}";
    shelly_sample sample;
    assert(mqtt_tasmota_valid_topic(topic) && !mqtt_tasmota_valid_topic("tasmotas") &&
        !mqtt_tasmota_valid_topic("fixture/+"));
    assert(mqtt_valid_name("broker.example") && !mqtt_valid_name("broker..example") &&
        !mqtt_valid_name("broker.example.") && !mqtt_valid_name("-broker.example"));
    assert(!mqtt_valid_ca("dGVzdCE=") && mqtt_valid_ca("dGVzdCE"));
    reset(0, 0, 0, 0);

    connected(3, 0);
    publish(1, "stat/fixture/STATUS5", "{\"StatusNET\":{\"Mac\":\"00:00:00:00:00:00\"}}");
    state("02:08:09:AA:BB:CC", off);
    publish(0, "stat/fixture/STATUS10", "{\"StatusSNS\":{\"ENERGY\":{\"Power\":0.5,\"Total\":0.25}}}");
    assert(mqtt_tasmota_status(&broker, mac, topic, &sample) == SHELLY_OK && !sample.on &&
        sample.power_known && sample.watts == 0.5 && sample.energy_known && sample.total_mwh == 250000);
    reset(1, 5, 0, 0);

    connected(4, 0);
    state("02:08:09:AA:BB:CC", off);
    publish(1, "stat/fixture/POWER", "OFF");
    publish(0, "stat/fixture/POWER1", "ON");
    assert(mqtt_tasmota_set(&broker, mac, topic, true, permitted) == SHELLY_OK);
    reset(1, 5, 1, 1);

    connected(4, 0);
    state("02:08:09:AA:BB:CC", off);
    allow_write = false;
    assert(mqtt_tasmota_set(&broker, mac, topic, true, permitted) == SHELLY_DEFERRED);
    reset(1, 4, 0, 1);

    connected(4, 0);
    state("02:08:09:AA:BB:CD", NULL);
    assert(mqtt_tasmota_set(&broker, mac, topic, true, permitted) == SHELLY_IDENTITY);
    reset(1, 3, 0, 0);

    connected(4, 0);
    state("02:08:09:AA:BB:CC", "{\"StatusSTS\":{\"POWER1\":\"OFF\",\"POWER2\":\"OFF\"}}");
    assert(mqtt_tasmota_set(&broker, mac, topic, true, permitted) == SHELLY_PROTOCOL);
    reset(1, 4, 0, 0);

    connected(4, 0);
    state("02:08:09:AA:BB:CC", off);
    assert(mqtt_tasmota_set(&broker, mac, topic, true, permitted) == SHELLY_NETWORK && now == shared_deadline());
    reset(1, 5, 1, 1);
    connected(3, 0);
    state("02:08:09:AA:BB:CC", "{\"StatusSTS\":{\"POWER\":\"ON\"}}");
    assert(mqtt_tasmota_status(&broker, mac, topic, &sample) == SHELLY_OK && sample.on && !sample.power_known);
    reset(1, 5, 0, 0);

    connected(3, 5);
    assert(mqtt_tasmota_status(&broker, mac, topic, &sample) == SHELLY_AUTH);
    reset(1, 1, 0, 0);

    connected(3, 0);
    static const unsigned char too_large[] = {0x30, 0x81, 0x0c}; // Remaining length 1537.
    append(too_large, sizeof(too_large));
    assert(mqtt_tasmota_status(&broker, mac, topic, &sample) == SHELLY_PROTOCOL);
    reset(1, 3, 0, 0);

    connected(3, 0);
    static const unsigned char overlong[] = {0x30, 0x81, 0x00};
    append(overlong, sizeof(overlong));
    assert(mqtt_tasmota_status(&broker, mac, topic, &sample) == SHELLY_PROTOCOL);
    reset(1, 3, 0, 0);

    strcpy(broker.tls_name, "broker.example");
    strcpy(broker.ca, "dGVzdCE");
    broker.port = 8883;
    tls_fails = true;
    assert(mqtt_tasmota_status(&broker, mac, topic, &sample) == SHELLY_NETWORK);
    reset(0, 0, 0, 0);
    tls_fails = false;
    connected(3, 0);
    state("02:08:09:AA:BB:CC", off);
    publish(0, "stat/fixture/STATUS10", "{\"StatusSNS\":{}}");
    assert(mqtt_tasmota_status(&broker, mac, topic, &sample) == SHELLY_OK && !sample.power_known);
    reset(1, 5, 0, 0);
    integration_profile custom = {.tasmota = {"ActivePower", "EnergyTotal", PROFILE_TASMOTA_INDEXED}};
    assert(integration_profile_activate(PROFILE_TASMOTA, &custom));
    connected(3, 0);
    state("02:08:09:AA:BB:CC", "{\"StatusSTS\":{\"POWER1\":\"ON\"}}");
    publish(0, "stat/fixture/STATUS10", "{\"StatusSNS\":{\"ENERGY\":{\"ActivePower\":12,\"EnergyTotal\":0.5}}}");
    assert(mqtt_tasmota_status(&broker, mac, topic, &sample) == SHELLY_OK && sample.on &&
        sample.power_known && sample.watts == 12 && sample.energy_known && sample.total_mwh == 500000);
    reset(1, 5, 0, 0);
    puts("Tasmota MQTT wire, broker identity, action and deadline fixtures passed");
}
