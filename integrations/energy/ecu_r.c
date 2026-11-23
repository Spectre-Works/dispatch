// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "ecu_r.h"

#include "esp_timer.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

enum { HEADER = 13, MIN_FRAME = 59, TIMEOUT_US = 5000000 };
static const char summary[] = "APS1100160001END\n";

static unsigned decimal(const char *text, size_t length)
{
    unsigned out = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return 0;
        out = out * 10 + text[i] - '0';
    }
    return out;
}

static uint32_t big_endian(const char *bytes, size_t length)
{
    uint32_t out = 0;
    for (size_t i = 0; i < length; ++i) out = (out << 8) | (unsigned char)bytes[i];
    return out;
}

bool ecu_r_decode(const char *frame, size_t size, ecu_reading *reading)
{
    if (size < MIN_FRAME || memcmp(frame, "APS11", 5) || memcmp(frame + 9, "0001", 4)) return false;
    const unsigned length = decimal(frame + 5, 4);
    if (length < MIN_FRAME || length > size || memcmp(frame + length - 3, "END", 3) ||
        (size != length && (size != length + 1 || frame[length] != '\n'))) return false;
    const unsigned version = decimal(frame + 52, 3);
    if (version < 6 || version >= sizeof(reading->firmware) || 55 + version > length - 3 ||
        memcmp(frame + 55, "ECU_R_", 6)) return false;
    for (unsigned i = 13; i < 25; ++i) {
        if (frame[i] < '0' || frame[i] > '9') return false;
    }
    for (unsigned i = 55; i < 55 + version; ++i) {
        const char c = frame[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    const uint16_t configured = big_endian(frame + 46, 2), online = big_endian(frame + 48, 2);
    if (online > configured) return false;
    *reading = (ecu_reading){
        .watts = big_endian(frame + 31, 4), .today_centi_kwh = big_endian(frame + 35, 4),
        .lifetime_deci_kwh = big_endian(frame + 27, 4), .inverters = configured, .online = online,
    };
    memcpy(reading->id, frame + 13, 12);
    memcpy(reading->firmware, frame + 55, version);
    return true;
}

void ecu_r_cancel(ecu_query *query)
{
    if (query->fd >= 0) close(query->fd);
    query->fd = -1;
}

static ecu_result finish(ecu_query *query, ecu_result result)
{
    ecu_r_cancel(query);
    return result;
}

ecu_result ecu_r_start(ecu_query *query, const char *ip, uint16_t port)
{
    *query = (ecu_query){.fd = -1, .deadline = esp_timer_get_time() + TIMEOUT_US};
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) return ECU_NETWORK;
    query->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (query->fd < 0 || fcntl(query->fd, F_SETFL, O_NONBLOCK) != 0) return finish(query, ECU_NETWORK);
    query->connected = connect(query->fd, (const struct sockaddr *)&address, sizeof(address)) == 0;
    return query->connected || errno == EINPROGRESS ? ECU_WAITING : finish(query, ECU_NETWORK);
}

ecu_result ecu_r_step(ecu_query *query, ecu_reading *reading)
{
    const int fd = query->fd;
    if (fd < 0 || esp_timer_get_time() >= query->deadline) return finish(query, ECU_NETWORK);
    if (!query->connected) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        struct timeval immediate = {0};
        int selected = select(fd + 1, NULL, &writable, NULL, &immediate);
        if (!selected) return ECU_WAITING;
        int error;
        socklen_t size = sizeof(error);
        if (selected < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error) {
            return finish(query, ECU_NETWORK);
        }
        query->connected = true;
    }
    if (query->sent < sizeof(summary) - 1) {
        int sent = send(fd, summary + query->sent, sizeof(summary) - 1 - query->sent, 0);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return ECU_WAITING;
        if (sent <= 0) return finish(query, ECU_NETWORK);
        query->sent += sent;
        if (query->sent < sizeof(summary) - 1) return ECU_WAITING;
    }
    int got = recv(fd, query->frame + query->used, sizeof(query->frame) - query->used, 0);
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return ECU_WAITING;
    if (got <= 0) return finish(query, query->used ? ECU_PROTOCOL : ECU_NETWORK);
    query->used += got;
    if (query->used < HEADER) return ECU_WAITING;
    const unsigned length = decimal(query->frame + 5, 4);
    if (memcmp(query->frame, "APS11", 5) || length < MIN_FRAME || length > sizeof(query->frame))
        return finish(query, ECU_PROTOCOL);
    if (query->used < length) return ECU_WAITING;
    return finish(query, ecu_r_decode(query->frame, query->used, reading) ? ECU_OK : ECU_PROTOCOL);
}
