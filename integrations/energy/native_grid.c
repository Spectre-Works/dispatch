// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "native_grid.h"

#include "esp_timer.h"
#include "local_tcp.h"
#include "modbus_tcp.h"
#include <math.h>
#include <string.h>
#include <unistd.h>

enum { ID_PREFIX = 4, SUNSPEC_MODELS = 32, GRID_LIMIT_WATTS = 1000000 };

native_grid_kind native_grid_type(const char *identity)
{
    if (!strncmp(identity, "sun:", ID_PREFIX)) return NATIVE_GRID_SUNSPEC;
    if (!strncmp(identity, "sv5:", ID_PREFIX)) return NATIVE_GRID_SOLARMAN;
    return NATIVE_GRID_NONE;
}

static uint32_t logger_serial(const char *text)
{
    const size_t length = strlen(text);
    if (!length || length > 10 || strspn(text, "0123456789") != length) return 0;
    uint64_t number = 0;
    for (; *text; ++text) number = number * 10 + (unsigned)(*text - '0');
    return number <= UINT32_MAX ? (uint32_t)number : 0;
}

static bool scaled_meter(unsigned model)
{
    return model >= 201 && model <= 204;
}

bool native_grid_valid(const char *identity, const native_grid_config *config)
{
    if (!config->port || !config->unit || config->unit > 247) return false;
    const native_grid_kind kind = native_grid_type(identity);
    if (kind == NATIVE_GRID_SUNSPEC) {
        const char *serial = identity + ID_PREFIX;
        const size_t length = strlen(serial);
        return length && length <= 32 &&
            strspn(serial, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") == length &&
            config->address <= 65468 && config->meter_index >= 1 && config->meter_index <= 16 &&
            (scaled_meter(config->model) || (config->model >= 211 && config->model <= 214));
    }
    return kind == NATIVE_GRID_SOLARMAN && logger_serial(identity + ID_PREFIX) &&
        (config->function == 3 || config->function == 4) && config->exponent >= -6 && config->exponent <= 6;
}

static int signed_word(uint16_t word)
{
    return word >= 0x8000 ? (int)word - 65536 : word;
}

static double scaled_power(int word, int exponent)
{
    static const double factor[] = {1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1, 1, 10, 100, 1000, 10000, 100000, 1000000};
    return word * factor[exponent + 6];
}

typedef struct {
    int fd;
    uint16_t transaction;
    uint8_t unit;
    int64_t deadline;
} sunspec_session;

static shelly_result sun_read(sunspec_session *session, uint16_t address, uint16_t *words, unsigned count)
{
    return modbus_read_connected(session->fd, ++session->transaction, session->unit, address, words, count, session->deadline);
}

static shelly_result sunspec_read(sunspec_session *session, const native_grid_config *config,
                                  const char *expected_serial, double *watts)
{
    uint16_t header[4];
    shelly_result result = sun_read(session, config->address, header, 4);
    if (result != SHELLY_OK) return result;
    if (header[0] != 0x5375 || header[1] != 0x6e53 || header[2] != 1 ||
        (header[3] != 65 && header[3] != 66)) return SHELLY_PROTOCOL;

    uint16_t serial_words[16];
    result = sun_read(session, config->address + 52, serial_words, 16);
    if (result != SHELLY_OK) return result;
    char serial[33] = {0};
    for (unsigned i = 0; i < 16; ++i) {
        serial[2 * i] = serial_words[i] >> 8;
        serial[2 * i + 1] = serial_words[i] & 0xff;
    }
    size_t end = strlen(serial);
    while (end && serial[end - 1] == ' ') serial[--end] = 0;
    if (strcmp(expected_serial, serial)) return SHELLY_IDENTITY;

    unsigned remaining = config->meter_index;
    unsigned address = config->address + 4u + header[3];
    for (unsigned i = 0; i < SUNSPEC_MODELS && address <= 65534; ++i) {
        result = sun_read(session, address, header, 2);
        if (result != SHELLY_OK) return result;
        const unsigned id = header[0], length = header[1];
        if (!id || id == 0xffff || !length || address + 2 + length > 65536) break;
        if (id == config->model && --remaining == 0) {
            if (scaled_meter(id)) {
                if (length != 105) return SHELLY_PROTOCOL;
                uint16_t fields[5];
                result = sun_read(session, address + 18, fields, 5);
                if (result != SHELLY_OK) return result;
                const int raw = signed_word(fields[0]), exponent = signed_word(fields[4]);
                if (raw == INT16_MIN || exponent < -6 || exponent > 6) return SHELLY_PROTOCOL;
                *watts = scaled_power(raw, exponent);
            } else {
                if (length != 124) return SHELLY_PROTOCOL;
                uint16_t fields[2];
                result = sun_read(session, address + 28, fields, 2);
                if (result != SHELLY_OK) return result;
                const uint32_t bits = ((uint32_t)fields[0] << 16) | fields[1];
                float reading;
                _Static_assert(sizeof(reading) == sizeof(bits), "SunSpec float32");
                memcpy(&reading, &bits, sizeof(reading));
                *watts = reading;
            }
            return SHELLY_OK;
        }
        address += 2 + length;
    }
    return SHELLY_IDENTITY;
}

static uint16_t rtu_crc(const uint8_t *data, unsigned count)
{
    uint16_t crc = 0xffff;
    for (unsigned i = 0; i < count; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xa001 : 0);
    }
    return crc;
}

static uint32_t little_u32(const uint8_t *bytes)
{
    return bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint8_t checksum(const uint8_t *bytes, unsigned count)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < count; ++i) sum += bytes[i];
    return sum;
}

static bool receive(int fd, uint8_t *bytes, size_t size, int64_t deadline)
{
    while (size) {
        const int n = local_tcp_read(fd, (char *)bytes, size, deadline);
        if (n <= 0) return false;
        bytes += n;
        size -= n;
    }
    return true;
}

static shelly_result solarman_read(int fd, const native_grid_config *config, uint32_t serial,
                                  time_t utc, int64_t deadline, double *watts)
{
    enum { HEADER_BYTES = 11, REQUEST_PREFIX = 15, RESPONSE_PREFIX = 14, READ_REPLY = 7 };
    const uint8_t sequence = (uint8_t)deadline;
    uint8_t request[HEADER_BYTES + REQUEST_PREFIX + 8 + 2] = {
        0xa5, 23, 0, 0x10, 0x45, sequence, 0, serial, serial >> 8, serial >> 16, serial >> 24, 0x02,
    };
    uint8_t *rtu = request + HEADER_BYTES + REQUEST_PREFIX;
    rtu[0] = config->unit;
    rtu[1] = config->function;
    rtu[2] = config->address >> 8;
    rtu[3] = config->address & 0xff;
    rtu[5] = 1;
    const uint16_t crc = rtu_crc(rtu, 6);
    rtu[6] = crc & 0xff;
    rtu[7] = crc >> 8;
    request[sizeof(request) - 2] = checksum(request + 1, sizeof(request) - 3);
    request[sizeof(request) - 1] = 0x15;

    uint8_t header[HEADER_BYTES], payload[RESPONSE_PREFIX + READ_REPLY + 2];
    if (!local_tcp_send(fd, (const char *)request, sizeof(request), deadline) ||
        !receive(fd, header, sizeof(header), deadline)) return SHELLY_NETWORK;
    if (header[0] != 0xa5 || header[1] != RESPONSE_PREFIX + READ_REPLY || header[2] ||
        header[3] != 0x10 || header[4] != 0x15 || header[5] != sequence) return SHELLY_PROTOCOL;
    if (little_u32(header + 7) != serial) return SHELLY_IDENTITY;
    if (!receive(fd, payload, sizeof(payload), deadline)) return SHELLY_NETWORK;
    if (payload[0] != 0x02 || payload[1] != 0x01 || payload[sizeof(payload) - 1] != 0x15 ||
        payload[sizeof(payload) - 2] != (uint8_t)(checksum(header + 1, sizeof(header) - 1) +
                                                checksum(payload, sizeof(payload) - 2))) return SHELLY_PROTOCOL;
    const uint64_t acquired = (uint64_t)little_u32(payload + 2) + little_u32(payload + 10);
    if (!utc || acquired + 10 < (uint64_t)utc || acquired > (uint64_t)utc + 5) return SHELLY_PROTOCOL;
    rtu = payload + RESPONSE_PREFIX;
    if (rtu[0] != config->unit || rtu[1] != config->function || rtu[2] != 2 ||
        rtu_crc(rtu, 5) != (rtu[5] | ((uint16_t)rtu[6] << 8))) return SHELLY_PROTOCOL;
    const int raw = signed_word(((uint16_t)rtu[3] << 8) | rtu[4]);
    if (raw == INT16_MIN) return SHELLY_PROTOCOL;
    *watts = scaled_power(raw, config->exponent);
    return SHELLY_OK;
}

shelly_result native_grid_power(const char *ip, const char *identity, const native_grid_config *config,
                                bool reverse, time_t utc, int32_t *watts)
{
    if (!native_grid_valid(identity, config)) return SHELLY_PROTOCOL;
    const int64_t deadline = esp_timer_get_time() + 700000;
    const int fd = local_tcp_open(ip, config->port, deadline);
    if (fd < 0) return SHELLY_NETWORK;
    sunspec_session session = {.fd = fd, .transaction = (uint16_t)deadline, .unit = config->unit, .deadline = deadline};
    double value = NAN;
    const shelly_result result = native_grid_type(identity) == NATIVE_GRID_SUNSPEC ?
        sunspec_read(&session, config, identity + ID_PREFIX, &value) :
        solarman_read(fd, config, logger_serial(identity + ID_PREFIX), utc, deadline, &value);
    close(fd);
    if (result != SHELLY_OK) return result;
    if (!isfinite(value) || fabs(value) > GRID_LIMIT_WATTS) return SHELLY_PROTOCOL;
    *watts = (int32_t)ceil(reverse ? -value : value);
    return SHELLY_OK;
}
