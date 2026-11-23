// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_TIMEZONE_H
#define DISPATCH_TIMEZONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum { TIMEZONE_NAME_BYTES = 40 };

typedef struct {
    const char *name;
    uint16_t first, count;
} dispatch_timezone;

const dispatch_timezone *timezone_at(size_t index);
const dispatch_timezone *timezone_find(const char *name);
bool timezone_local(const dispatch_timezone *zone, int fixed_offset, time_t utc,
                    uint32_t *day, uint32_t *minute);
time_t timezone_transition(const dispatch_timezone *zone, time_t utc);
#endif
