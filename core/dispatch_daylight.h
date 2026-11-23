// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_DAYLIGHT_H
#define DISPATCH_DAYLIGHT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { DAYLIGHT_UNKNOWN, DAYLIGHT_RISE_SET, DAYLIGHT_ALL_DAY, DAYLIGHT_ALL_NIGHT } dispatch_daylight_kind;

typedef struct {
    int64_t sunrise, sunset;
    dispatch_daylight_kind kind;
} dispatch_daylight;

bool dispatch_solar_location_valid(int32_t latitude_e4, int32_t longitude_e4);
int64_t dispatch_solar_day(int64_t epoch, int32_t longitude_e4);
dispatch_daylight dispatch_sun_times(int64_t day, int32_t latitude_e4, int32_t longitude_e4);
bool dispatch_in_daylight(dispatch_daylight sun, int64_t epoch);

#endif
