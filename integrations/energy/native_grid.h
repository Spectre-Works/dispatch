// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_NATIVE_GRID_H
#define DISPATCH_NATIVE_GRID_H

#include "shelly.h"
#include <time.h>

typedef enum { NATIVE_GRID_NONE, NATIVE_GRID_SUNSPEC, NATIVE_GRID_SOLARMAN } native_grid_kind;
typedef struct {
    uint16_t port, address, model;
    uint8_t unit, meter_index, function;
    int8_t exponent;
} native_grid_config;

native_grid_kind native_grid_type(const char *identity);
bool native_grid_valid(const char *identity, const native_grid_config *config);
shelly_result native_grid_power(const char *ip, const char *identity, const native_grid_config *config,
                                bool reverse, time_t utc, int32_t *watts);
#endif
