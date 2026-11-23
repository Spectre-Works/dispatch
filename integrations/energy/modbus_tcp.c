// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "modbus_tcp.h"

#include "local_tcp.h"
#include <unistd.h>

static uint16_t read_word(const uint8_t *bytes)
{
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static void put_word(uint8_t *bytes, uint16_t word)
{
    bytes[0] = word >> 8;
    bytes[1] = word;
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

enum { MODBUS_SESSION_WORDS = 16 };

static shelly_result exchange_connected(int fd, uint16_t transaction, uint8_t unit, uint16_t address,
                                        uint16_t *read, const uint16_t *write, unsigned count, int64_t deadline)
{
    if (!count || count > (write ? MODBUS_TCP_WORDS : MODBUS_SESSION_WORDS) ||
        (unsigned)address + count > 65536) return SHELLY_PROTOCOL;
    const uint8_t fn = write ? 16 : 3;
    uint8_t request[13 + 2 * MODBUS_TCP_WORDS] = {0};
    put_word(request, transaction);
    put_word(request + 4, write ? 7 + 2 * count : 6);
    request[6] = unit;
    request[7] = fn;
    put_word(request + 8, address);
    put_word(request + 10, count);
    if (write) {
        request[12] = 2 * count;
        for (unsigned i = 0; i < count; ++i) put_word(request + 13 + 2 * i, write[i]);
    }

    uint8_t header[7], payload[2 + 2 * MODBUS_SESSION_WORDS];
    const bool got_header = local_tcp_send(fd, (const char *)request, write ? 13 + 2 * count : 12, deadline) &&
        receive(fd, header, sizeof(header), deadline);
    const unsigned length = got_header ? read_word(header + 4) : 0;
    const bool valid_header = got_header && length >= 3 && length <= sizeof(payload) + 1 &&
        read_word(header) == transaction && !read_word(header + 2) && header[6] == unit;
    const bool got_body = valid_header && receive(fd, payload, length - 1, deadline);
    if (!got_header || (valid_header && !got_body)) return SHELLY_NETWORK;
    if (!valid_header || payload[0] != fn) return SHELLY_PROTOCOL;
    if (write) {
        return length == 6 && read_word(payload + 1) == address && read_word(payload + 3) == count
            ? SHELLY_OK : SHELLY_PROTOCOL;
    }
    if (length != 3 + 2 * count || payload[1] != 2 * count) return SHELLY_PROTOCOL;
    for (unsigned i = 0; i < count; ++i) read[i] = read_word(payload + 2 + 2 * i);
    return SHELLY_OK;
}

shelly_result modbus_read_connected(int fd, uint16_t transaction, uint8_t unit, uint16_t address,
                                    uint16_t words[], unsigned count, int64_t deadline)
{
    return exchange_connected(fd, transaction, unit, address, words, NULL, count, deadline);
}

// A single request per connection. No unconfirmed write is sent again here.
static shelly_result exchange(const char *ip, uint16_t port, uint8_t unit, uint16_t address,
                              uint16_t *read, const uint16_t *write, unsigned count, int64_t deadline)
{
    if (!count || count > MODBUS_TCP_WORDS || (unsigned)address + count > 65536) return SHELLY_PROTOCOL;
    const int fd = local_tcp_open(ip, port, deadline);
    if (fd < 0) return SHELLY_NETWORK;
    const shelly_result result = exchange_connected(fd, (uint16_t)deadline, unit, address, read, write, count, deadline);
    close(fd);
    return result;
}

shelly_result modbus_read(const char *ip, uint16_t port, uint8_t unit, uint16_t address,
                          uint16_t words[], unsigned count, int64_t deadline)
{
    return exchange(ip, port, unit, address, words, NULL, count, deadline);
}

shelly_result modbus_write(const char *ip, uint16_t port, uint8_t unit, uint16_t address,
                           const uint16_t words[], unsigned count, int64_t deadline)
{
    return exchange(ip, port, unit, address, NULL, words, count, deadline);
}
