// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_FRONIUS_H
#define DISPATCH_FRONIUS_H

#include "shelly.h"

typedef struct {
    bool solar_known, grid_known;
    int32_t grid_watts;
    uint32_t solar_watts;
} fronius_sample;

// Uses the inverter's private LAN Solar API. Positive grid power is import.
shelly_result fronius_power_flow(const char *ip, fronius_sample *sample);

#endif
