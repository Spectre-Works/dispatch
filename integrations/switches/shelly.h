// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_SHELLY_H
#define DISPATCH_SHELLY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SHELLY_OK, SHELLY_NETWORK, SHELLY_AUTH, SHELLY_PROTOCOL, SHELLY_IDENTITY, SHELLY_DEFERRED
} shelly_result;

typedef struct {
    char ip[16];
    char identity[40];
    char password[65];
} shelly_target;

typedef struct {
    bool on, power_known, energy_known;
    double watts;
    uint64_t total_mwh;
    unsigned timer_seconds;
    bool timer_active, flip_back_timer;
} shelly_sample;

bool shelly_valid_identity(const char *identity);
bool shelly_valid_relay_channel(const char *identity, unsigned channel);
bool shelly_valid_grid_identity(const char *identity, unsigned channel);
bool shelly_valid_credentials(const char *identity, const char *password);
bool shelly_identify(shelly_target *target);
shelly_result shelly_status_channel(const shelly_target *target, unsigned channel, shelly_sample *sample);
shelly_result shelly_grid_power_channel(const shelly_target *target, unsigned channel, int32_t *watts);
shelly_result shelly_set_channel(const shelly_target *target, unsigned channel, bool on, bool (*still_allowed)(void));
shelly_result shelly_timed_on_channel(const shelly_target *target, unsigned channel, unsigned seconds,
                                     bool (*still_allowed)(void));
shelly_result shelly_status(const shelly_target *target, shelly_sample *sample);
shelly_result shelly_grid_power(const shelly_target *target, int32_t *watts);
shelly_result shelly_set(const shelly_target *target, bool on, bool (*still_allowed)(void));
shelly_result shelly_timed_on(const shelly_target *target, unsigned seconds, bool (*still_allowed)(void));
#endif
