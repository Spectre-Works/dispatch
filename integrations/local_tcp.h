// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LOCAL_TCP_H
#define DISPATCH_LOCAL_TCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int local_tcp_open(const char *ip, unsigned port, int64_t deadline);
bool local_tcp_send(int fd, const char *bytes, size_t size, int64_t deadline);
int local_tcp_read(int fd, char *bytes, size_t capacity, int64_t deadline);
#endif
