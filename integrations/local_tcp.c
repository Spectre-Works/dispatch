// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "local_tcp.h"

#include "esp_timer.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <fcntl.h>

static bool ready(int fd, bool write, int64_t deadline)
{
    int64_t left = deadline - esp_timer_get_time();
    if (left <= 0) {
        return false;
    }
    struct timeval timeout = {.tv_sec = left / 1000000, .tv_usec = left % 1000000};
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    return select(fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &timeout) == 1;
}

int local_tcp_open(const char *ip, unsigned port, int64_t deadline)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0) {
        if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
            return fd;
        }
        if (errno == EINPROGRESS && ready(fd, true, deadline)) {
            int error;
            socklen_t size = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error) {
                return fd;
            }
        }
    }
    close(fd);
    return -1;
}

bool local_tcp_send(int fd, const char *bytes, size_t size, int64_t deadline)
{
    while (size && ready(fd, true, deadline)) {
        int n = send(fd, bytes, size, 0);
        if (n <= 0) {
            return false;
        }
        bytes += n;
        size -= n;
    }
    return size == 0;
}

int local_tcp_read(int fd, char *bytes, size_t capacity, int64_t deadline)
{
    return ready(fd, false, deadline) ? recv(fd, bytes, capacity, 0) : -1;
}
