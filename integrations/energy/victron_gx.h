// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_VICTRON_GX_H
#define DISPATCH_VICTRON_GX_H

#include "shelly.h"

enum { VICTRON_TARGET_MAX = 10000 };

unsigned victron_grid_phases(const char *identity);
shelly_result victron_grid_power(const char *ip, unsigned phases, int32_t *watts);
shelly_result victron_setpoint(const char *ip, int32_t *watts);
shelly_result victron_set_grid_target(const char *ip, int32_t expected, int32_t watts,
                                      bool (*allowed)(void));
#endif
