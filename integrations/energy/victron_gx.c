// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "equipment_writes.h"
#include "victron_gx.h"
#include "integration_profile.h"

#include "esp_timer.h"
#include "modbus_tcp.h"
#include <string.h>

enum { GX_PORT = 502, GX_SYSTEM = 100, GX_ACTIVE_INPUT = 826,
       GX_VOLATILE_GRID_TARGET = 2716, GX_ESS_MODE = 2902, GX_DYNAMIC_ESS = 5423,
       GX_GRID_POWER_LIMIT = 1000000 };

unsigned victron_grid_phases(const char *identity)
{
    return strlen(identity) == 10 && !strncmp(identity, "victron-", 8) &&
        identity[8] >= '1' && identity[8] <= '3' && identity[9] == 'p' ? identity[8] - '0' : 0;
}

static shelly_result read_system(const char *ip, uint16_t address, uint16_t *words,
                                  unsigned count, int64_t deadline)
{
    return modbus_read(ip, GX_PORT, GX_SYSTEM, address, words, count, deadline);
}

static int32_t signed_watts(const uint16_t words[2])
{
    const uint32_t encoded = ((uint32_t)words[0] << 16) | words[1];
    return encoded <= INT32_MAX ? (int32_t)encoded : -1 - (int32_t)(UINT32_MAX - encoded);
}

static shelly_result check_grid_input(const char *ip, int64_t deadline)
{
    uint16_t source;
    const shelly_result result = read_system(ip, GX_ACTIVE_INPUT, &source, 1, deadline);
    return result == SHELLY_OK && source != 1 ? SHELLY_PROTOCOL : result;
}

shelly_result victron_grid_power(const char *ip, unsigned phases, int32_t *watts)
{
    if (!phases || phases > 3) return SHELLY_PROTOCOL;
    const int64_t deadline = esp_timer_get_time() + 700000;
    uint16_t values[6];
    const profile_victron *mapping = integration_profile_get(PROFILE_VICTRON);
    shelly_result result = read_system(ip, mapping->first_register, values, 2 * phases, deadline);
    if (result != SHELLY_OK) return result;
    result = check_grid_input(ip, deadline);
    if (result != SHELLY_OK) return result;
    int32_t total = 0;
    for (unsigned i = 0; i < phases; ++i) {
        uint16_t *words = values + 2 * i;
        const uint16_t ordered[2] = {words[mapping->low_first], words[!mapping->low_first]};
        const int32_t phase = signed_watts(ordered);
        if (phase < -GX_GRID_POWER_LIMIT || phase > GX_GRID_POWER_LIMIT) return SHELLY_PROTOCOL;
        total += phase;
    }
    if (total < -GX_GRID_POWER_LIMIT || total > GX_GRID_POWER_LIMIT) return SHELLY_PROTOCOL;
    *watts = total;
    return SHELLY_OK;
}

static shelly_result read_setpoint(const char *ip, int32_t *watts, int64_t deadline)
{
    uint16_t words[2];
    const shelly_result result = read_system(ip, GX_VOLATILE_GRID_TARGET, words, 2, deadline);
    if (result != SHELLY_OK) return result;
    *watts = signed_watts(words);
    return SHELLY_OK;
}

shelly_result victron_setpoint(const char *ip, int32_t *watts)
{
    return read_setpoint(ip, watts, esp_timer_get_time() + 500000);
}

shelly_result victron_set_grid_target(const char *ip, int32_t expected, int32_t watts,
                                      bool (*allowed)(void))
{
    if (!equipment_writes_allowed()) return SHELLY_DEFERRED;
    if (watts < -VICTRON_TARGET_MAX || watts > VICTRON_TARGET_MAX) return SHELLY_PROTOCOL;
    const int64_t deadline = esp_timer_get_time() + 1100000;
    uint16_t mode, dynamic;
    int32_t observed;
    shelly_result result = read_system(ip, GX_ESS_MODE, &mode, 1, deadline);
    if (result != SHELLY_OK) return result;
    if (mode != 1 && mode != 2) return SHELLY_PROTOCOL;
    result = read_system(ip, GX_DYNAMIC_ESS, &dynamic, 1, deadline);
    if (result != SHELLY_OK) return result;
    if (dynamic != 0) return SHELLY_PROTOCOL;
    result = check_grid_input(ip, deadline);
    if (result != SHELLY_OK) return result;
    result = read_setpoint(ip, &observed, deadline);
    if (result != SHELLY_OK) return result;
    if (expected != observed || (allowed && !allowed())) return SHELLY_DEFERRED;
    const uint32_t encoded = (uint32_t)watts;
    const uint16_t words[] = {encoded >> 16, encoded};
    return modbus_write(ip, GX_PORT, GX_SYSTEM, GX_VOLATILE_GRID_TARGET, words, 2, deadline);
}
