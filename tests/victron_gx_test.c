// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "local_tcp.h"
#include "modbus_tcp.h"
#include "victron_gx.h"
#include "integration_profile.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    REPLY_NORMAL,
    REPLY_EXCEPTION,
    REPLY_WRONG_TRANSACTION,
    REPLY_WRONG_UNIT,
    REPLY_BAD_ECHO,
    REPLY_CUT_SHORT,
    FAIL_CONNECT,
} fixture_fault;

typedef struct {
    uint16_t address;
    unsigned count;
    uint16_t words[MODBUS_TCP_WORDS];
    bool write;
    fixture_fault fault;
} request_fixture;

static const char address[] = "192.168.53.18";
static const int64_t now = 0x123456;
static request_fixture script[5];
static unsigned next_request, request_count, writes, authorization_checks;
static int fd = -1;
static uint8_t response[32];
static size_t bytes, cursor;
static const request_fixture *fixture;

int64_t esp_timer_get_time(void) { return now; }

static void verify_closed(void)
{
    if (fd < 0) return;
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    fd = -1;
}

static void prepare(const request_fixture *requests, unsigned count)
{
    assert(next_request == request_count && count <= sizeof(script) / sizeof(*script));
    verify_closed();
    if (count) memcpy(script, requests, count * sizeof(*script));
    request_count = count;
    next_request = writes = authorization_checks = 0;
}

static unsigned word(const uint8_t *data)
{
    return (unsigned)data[0] * 256 + data[1];
}

static void store_word(uint8_t *data, unsigned value)
{
    data[0] = value >> 8;
    data[1] = value;
}

int local_tcp_open(const char *ip, unsigned port, int64_t deadline)
{
    verify_closed();
    assert(!strcmp(ip, address) && port == 502 && deadline > now);
    assert(next_request < request_count);
    fixture = &script[next_request++];
    if (script[0].address == 908) assert(deadline == now + 700000);
    if (fixture->fault == FAIL_CONNECT) return -1;
    fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    cursor = bytes = 0;
    return fd;
}

bool local_tcp_send(int socket, const char *request, size_t size, int64_t deadline)
{
    assert(socket == fd && !cursor && !bytes);
    const uint8_t *wire = (const uint8_t *)request;
    assert(size == (fixture->write ? 13 + 2 * fixture->count : 12));
    assert(word(wire) == (uint16_t)deadline && word(wire + 2) == 0);
    assert(word(wire + 4) == size - 6 && wire[6] == 100 && wire[7] == (fixture->write ? 16 : 3));
    assert(word(wire + 8) == fixture->address && word(wire + 10) == fixture->count);
    if (fixture->write) {
        ++writes;
        assert(wire[12] == 2 * fixture->count);
        for (unsigned i = 0; i < fixture->count; ++i) {
            assert(word(wire + 13 + 2 * i) == fixture->words[i]);
        }
    }

    memcpy(response, wire, 8);
    if (fixture->fault == REPLY_WRONG_TRANSACTION) response[1] ^= 1;
    if (fixture->fault == REPLY_WRONG_UNIT) response[6] = 101;
    if (fixture->fault == REPLY_EXCEPTION) {
        response[7] |= 0x80;
        response[8] = 2;  // Illegal data address
        bytes = 9;
    } else if (fixture->write) {
        memcpy(response + 8, wire + 8, 4);
        if (fixture->fault == REPLY_BAD_ECHO) response[11] ^= 1;
        bytes = 12;
    } else {
        response[8] = 2 * fixture->count;
        for (unsigned i = 0; i < fixture->count; ++i) {
            store_word(response + 9 + 2 * i, fixture->words[i]);
        }
        bytes = 9 + 2 * fixture->count;
    }
    store_word(response + 4, bytes - 6);
    if (fixture->fault == REPLY_CUT_SHORT) --bytes;
    return true;
}

int local_tcp_read(int socket, char *out, size_t capacity, int64_t deadline)
{
    assert(socket == fd && deadline > now);
    if (cursor == bytes) return 0;
    size_t take = bytes - cursor;
    if (take > 2) take = 2;
    if (take > capacity) take = capacity;
    memcpy(out, response + cursor, take);
    cursor += take;
    return take;
}

static bool permitted(void)
{
    ++authorization_checks;
    return true;
}

static bool paused(void)
{
    ++authorization_checks;
    return false;
}

static void grid_phases(void)
{
    int32_t watts = 100;
    assert(victron_grid_phases("victron-1p") == 1);
    assert(victron_grid_phases("victron-3p") == 3);
    assert(!victron_grid_phases("victron-0p") && !victron_grid_phases("victron-4p"));
    assert(!victron_grid_phases("victron-2p-extra") && !victron_grid_phases("other-2p"));
    assert(victron_grid_power(address, 4, &watts) == SHELLY_PROTOCOL && watts == 100);

    request_fixture sample[] = {
        {.address = 908, .count = 6, .words = {0, 38500, 0xffff, 0x55e2, 0, 300}},
        {.address = 826, .count = 1, .words = {1}},
    };
    prepare(sample, 2);
    assert(victron_grid_power(address, 3, &watts) == SHELLY_OK && watts == -4750);
    sample[0].count = 2;
    prepare(sample, 2);
    assert(victron_grid_power(address, 1, &watts) == SHELLY_OK && watts == 38500);

    sample[1].words[0] = 2;
    prepare(sample, 2);
    assert(victron_grid_power(address, 1, &watts) == SHELLY_PROTOCOL && watts == 38500);
    sample[1].words[0] = 1;
    sample[1].fault = FAIL_CONNECT;
    prepare(sample, 2);
    assert(victron_grid_power(address, 1, &watts) == SHELLY_NETWORK && watts == 38500);
    sample[1].fault = REPLY_NORMAL;
    sample[0].words[2] = 0x8000;
    sample[0].count = 4;
    prepare(sample, 2);
    assert(victron_grid_power(address, 2, &watts) == SHELLY_PROTOCOL);
    sample[0].count = 2;
    prepare(sample, 2);
    assert(victron_grid_power(address, 1, &watts) == SHELLY_OK && watts == 38500);
}

static void custom_grid_register(void)
{
    integration_profile custom = {.victron = {500, true}};
    assert(integration_profile_activate(PROFILE_VICTRON, &custom));
    const request_fixture sample[] = {
        {.address = 500, .count = 2, .words = {0xfff6, 0xffff}},
        {.address = 826, .count = 1, .words = {1}},
    };
    prepare(sample, 2);
    int32_t watts;
    assert(victron_grid_power(address, 1, &watts) == SHELLY_OK && watts == -10);
}

static void setpoint(void)
{
    request_fixture command[] = {
        {.address = 2902, .count = 1, .words = {2}},
        {.address = 5423, .count = 1, .words = {0}},
        {.address = 826, .count = 1, .words = {1}},
        {.address = 2716, .count = 2, .words = {0, 50}},
        {.address = 2716, .count = 2, .words = {0xffff, 0xf63c}, .write = true},
    };

    prepare(NULL, 0);
    assert(victron_set_grid_target(address, 50, VICTRON_TARGET_MAX + 1, permitted) == SHELLY_PROTOCOL);
    assert(victron_set_grid_target(address, 50, -VICTRON_TARGET_MAX - 1, permitted) == SHELLY_PROTOCOL);
    assert(!authorization_checks && !writes);

    command[0].words[0] = 3;
    prepare(command, 1);
    assert(victron_set_grid_target(address, 50, -2500, permitted) == SHELLY_PROTOCOL);
    assert(!authorization_checks && !writes);
    command[0].words[0] = 1;
    command[1].words[0] = 4;
    prepare(command, 2);
    assert(victron_set_grid_target(address, 50, -2500, permitted) == SHELLY_PROTOCOL);
    assert(!authorization_checks && !writes);
    command[1].words[0] = 0;
    command[2].words[0] = 240;
    prepare(command, 3);
    assert(victron_set_grid_target(address, 50, -2500, permitted) == SHELLY_PROTOCOL);
    assert(!authorization_checks && !writes);
    command[2].words[0] = 1;

    prepare(command, 4);
    assert(victron_set_grid_target(address, 51, -2500, permitted) == SHELLY_DEFERRED);
    assert(!authorization_checks && !writes);
    prepare(command, 4);
    assert(victron_set_grid_target(address, 50, -2500, paused) == SHELLY_DEFERRED);
    assert(authorization_checks == 1 && !writes);

    prepare(command, 5);
    assert(victron_set_grid_target(address, 50, -2500, permitted) == SHELLY_OK);
    assert(authorization_checks == 1 && writes == 1);
    command[4].fault = REPLY_BAD_ECHO;
    prepare(command, 5);
    assert(victron_set_grid_target(address, 50, -2500, permitted) == SHELLY_PROTOCOL && writes == 1);
    command[4].fault = REPLY_CUT_SHORT;
    prepare(command, 5);
    assert(victron_set_grid_target(address, 50, -2500, permitted) == SHELLY_NETWORK && writes == 1);

    request_fixture verify = {.address = 2716, .count = 2, .words = {0xffff, 0xf63c}, .fault = FAIL_CONNECT};
    int32_t observed = 50;
    prepare(&verify, 1);
    assert(victron_setpoint(address, &observed) == SHELLY_NETWORK && observed == 50 && !writes);
    verify.fault = REPLY_NORMAL;
    prepare(&verify, 1);
    assert(victron_setpoint(address, &observed) == SHELLY_OK && observed == -2500 && !writes);
}

static void protocol_failures(void)
{
    request_fixture sample = {.address = 2716, .count = 2, .words = {0, 50}};
    const fixture_fault failures[] = {REPLY_EXCEPTION, REPLY_WRONG_TRANSACTION, REPLY_WRONG_UNIT};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(*failures); ++i) {
        sample.fault = failures[i];
        prepare(&sample, 1);
        int32_t watts = -15;
        assert(victron_setpoint(address, &watts) == SHELLY_PROTOCOL && watts == -15 && !writes);
    }
    prepare(NULL, 0);
    uint16_t words[MODBUS_TCP_WORDS + 1];
    assert(modbus_read(address, 502, 100, 0, words, 0, now + 100) == SHELLY_PROTOCOL);
    assert(modbus_read(address, 502, 100, 0, words, MODBUS_TCP_WORDS + 1, now + 100) == SHELLY_PROTOCOL);
    assert(modbus_read(address, 502, 100, 65535, words, 2, now + 100) == SHELLY_PROTOCOL);
}

int main(void)
{
    grid_phases();
    custom_grid_register();
    setpoint();
    protocol_failures();
    assert(next_request == request_count);
    verify_closed();
    puts("DISPATCH Victron GX Modbus fixtures passed");
}
