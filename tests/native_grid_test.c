// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "native_grid.h"
#include "local_tcp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t reply[64];
static size_t reply_size, bytes_read;
static unsigned opens, exchanges;
static uint16_t meter_model;
static bool solarman, invalid, stale, wrong_serial;
enum { CLOCK = 1760000000 };
static const uint32_t serial = 123456;

int64_t esp_timer_get_time(void) { return 100; }

static uint16_t word(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static void put_word(uint8_t *data, uint16_t value)
{
    data[0] = value >> 8;
    data[1] = value & 255;
}

static void put_le32(uint8_t *data, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) data[i] = value >> (8 * i);
}

static uint8_t sum(const uint8_t *data, size_t len)
{
    unsigned total = 0;
    for (size_t i = 0; i < len; ++i) total += data[i];
    return total & 255;
}

int local_tcp_open(const char *ip, unsigned port, int64_t deadline)
{
    assert(!strcmp(ip, "192.0.2.10") && deadline == 700100 && port == (solarman ? 8899 : 1502));
    ++opens;
    return 991;
}

static void modbus_fixture(const uint8_t *request, size_t size)
{
    assert(size == 12 && word(request + 2) == 0 && word(request + 4) == 6 && request[6] == 1 && request[7] == 3);
    const unsigned address = word(request + 8), count = word(request + 10);
    memcpy(reply, request, 8);
    put_word(reply + 4, 3 + 2 * count);
    reply[8] = 2 * count;
    uint8_t *data = reply + 9;
    reply_size = 9 + 2 * count;
    switch (address) {
    case 40000:
        assert(count == 4);
        memcpy(data, "SunS\x00\x01\x00\x41", 8);
        break;
    case 40052:
        assert(count == 16);
        memcpy(data, wrong_serial ? "OtherABC " : "MeterABC ", 9);
        break;
    case 40069:
        assert(count == 2);
        put_word(data, 103);
        put_word(data + 2, 50);
        break;
    case 40121:
        assert(count == 2);
        put_word(data, 5);
        put_word(data + 2, 65);
        break;
    case 40188:
        assert(count == 2);
        put_word(data, meter_model);
        put_word(data + 2, meter_model == 203 ? 105 : 124);
        break;
    case 40206:
        assert(count == 5);
        put_word(data, invalid ? 0x8000 : (uint16_t)-245);
        put_word(data + 8, 1);
        break;
    case 40216:
        assert(count == 2);
        put_word(data, invalid ? 0x7fc0 : 0xc2fb);
        break;
    default:
        assert(0 && "Unexpected SunSpec address");
    }
}

static void solarman_fixture(const uint8_t *request, size_t size)
{
    assert(size == 36 && request[0] == 0xa5 && request[1] == 23 && request[3] == 0x10 && request[4] == 0x45);
    assert(request[11] == 2 && request[34] == sum(request + 1, 33) && request[35] == 0x15);
    // RTU read of one holding register at address 0x0058.
    static const uint8_t request_rtu[] = {1, 3, 0, 0x58, 0, 1, 0x05, 0xd9};
    assert(!memcmp(request + 26, request_rtu, sizeof(request_rtu)));
    reply[0] = 0xa5;
    reply[1] = 21;
    reply[3] = 0x10;
    reply[4] = 0x15;
    reply[5] = request[5];
    put_le32(reply + 7, serial + wrong_serial);
    reply[11] = 2;
    reply[12] = 1;
    put_le32(reply + 13, 100);
    put_le32(reply + 21, CLOCK - 100 - (stale ? 30 : 0));
    // -2145 raw watts, followed by its independent RTU CRC.
    static const uint8_t response_rtu[] = {1, 3, 2, 0xf7, 0x9f, 0xbe, 0x1c};
    memcpy(reply + 25, response_rtu, sizeof(response_rtu));
    if (invalid) reply[31] ^= 1;
    reply[32] = sum(reply + 1, 31);
    reply[33] = 0x15;
    reply_size = 34;
}

bool local_tcp_send(int fd, const char *bytes, size_t size, int64_t deadline)
{
    assert(fd == 991 && deadline == 700100 && bytes_read == reply_size);
    bytes_read = 0;
    memset(reply, 0, sizeof(reply));
    if (solarman) solarman_fixture((const uint8_t *)bytes, size);
    else modbus_fixture((const uint8_t *)bytes, size);
    ++exchanges;
    return true;
}

int local_tcp_read(int fd, char *destination, size_t capacity, int64_t deadline)
{
    assert(fd == 991 && deadline == 700100);
    size_t count = reply_size - bytes_read;
    if (count > 3) count = 3;
    if (count > capacity) count = capacity;
    memcpy(destination, reply + bytes_read, count);
    bytes_read += count;
    return count;
}

static void reset(void)
{
    opens = exchanges = 0;
    bytes_read = reply_size = 0;
}

int main(void)
{
    const char *ip = "192.0.2.10";
    native_grid_config config = {.port = 1502, .unit = 1, .address = 40000, .model = 203, .meter_index = 1};
    int32_t watts = 999;
    meter_model = 203;
    assert(native_grid_power(ip, "sun:MeterABC", &config, false, CLOCK, &watts) == SHELLY_OK && watts == -2450);
    assert(opens == 1 && exchanges == 6);
    reset();
    wrong_serial = true;
    assert(native_grid_power(ip, "sun:MeterABC", &config, false, CLOCK, &watts) == SHELLY_IDENTITY);
    assert(watts == -2450 && exchanges == 2);
    wrong_serial = false;
    config.model = meter_model = 213;
    reset();
    assert(native_grid_power(ip, "sun:MeterABC", &config, true, CLOCK, &watts) == SHELLY_OK && watts == 126);
    invalid = true;
    reset();
    assert(native_grid_power(ip, "sun:MeterABC", &config, false, CLOCK, &watts) == SHELLY_PROTOCOL && watts == 126);
    assert(!native_grid_valid("sun:serial with spaces", &config));

    solarman = true;
    invalid = false;
    config = (native_grid_config){.port = 8899, .unit = 1, .address = 88, .function = 3, .exponent = -1};
    reset();
    assert(native_grid_power(ip, "sv5:123456", &config, false, CLOCK, &watts) == SHELLY_OK && watts == -214);
    assert(opens == 1 && exchanges == 1);
    stale = true;
    reset();
    assert(native_grid_power(ip, "sv5:123456", &config, false, CLOCK, &watts) == SHELLY_PROTOCOL && watts == -214);
    stale = false;
    invalid = true;
    reset();
    assert(native_grid_power(ip, "sv5:123456", &config, false, CLOCK, &watts) == SHELLY_PROTOCOL && watts == -214);
    assert(!native_grid_valid("sv5:4294967296", &config));
    puts("Native SunSpec and Solarman grid meter protocol checks passed");
}
