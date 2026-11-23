// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "dispatch_timezone.h"

#include <string.h>

typedef struct {
    uint32_t epoch;
    int16_t minutes;
} timezone_change;

#include "timezone_data.inc"

const dispatch_timezone *timezone_at(size_t index)
{
    return index < sizeof(timezones) / sizeof(*timezones) ? &timezones[index] : NULL;
}

const dispatch_timezone *timezone_find(const char *name)
{
    for (const dispatch_timezone *zone = timezones; zone < timezones + sizeof(timezones) / sizeof(*timezones); ++zone) {
        if (!strcmp(zone->name, name)) {
            return zone;
        }
    }
    return NULL;
}

static size_t change_index(const dispatch_timezone *zone, time_t utc)
{
    const timezone_change *changes = &timezone_changes[zone->first];
    size_t low = 0, high = zone->count;
    while (high - low > 1) {
        const size_t middle = low + (high - low) / 2;
        if (utc >= changes[middle].epoch) low = middle;
        else high = middle;
    }
    return low;
}

time_t timezone_transition(const dispatch_timezone *zone, time_t utc)
{
    if (!zone || utc < TIMEZONE_BEGIN || utc >= TIMEZONE_END) return 0;
    const size_t next = change_index(zone, utc) + 1;
    return next < zone->count ? timezone_changes[zone->first + next].epoch : 0;
}

bool timezone_local(const dispatch_timezone *zone, int fixed_offset, time_t utc,
                    uint32_t *day, uint32_t *minute)
{
    if (!utc || (zone && (utc < TIMEZONE_BEGIN || utc >= TIMEZONE_END))) {
        return false;
    }
    int offset = fixed_offset;
    if (zone) {
        offset = timezone_changes[zone->first + change_index(zone, utc)].minutes;
    }
    const int64_t local = (int64_t)utc + offset * 60;
    *day = local / 86400;
    if (minute) {
        *minute = local % 86400 / 60;
    }
    return true;
}
