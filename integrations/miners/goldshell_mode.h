// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_GOLDSHELL_MODE_H
#define DISPATCH_GOLDSHELL_MODE_H

#include <stdbool.h>

enum { GOLDSHELL_PASSWORD_BYTES = 49 };
typedef enum {
    GOLDSHELL_MODE_UNKNOWN, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_LOW, GOLDSHELL_MODE_BALANCE, GOLDSHELL_MODE_IDLE,
} goldshell_mode;
typedef enum {
    GOLDSHELL_MODE_UNCHECKED, GOLDSHELL_MODE_READY, GOLDSHELL_MODE_UNSUPPORTED, GOLDSHELL_MODE_OFFLINE,
    GOLDSHELL_MODE_AUTH, GOLDSHELL_MODE_CHANGED, GOLDSHELL_MODE_UNCONFIRMED, GOLDSHELL_MODE_DEFERRED,
    GOLDSHELL_MODE_VERIFYING, GOLDSHELL_MODE_ALGORITHM, GOLDSHELL_MODE_HASHRATE,
    GOLDSHELL_MODE_ACKNOWLEDGED,
} goldshell_mode_status;

typedef struct {
    goldshell_mode_status status;
    goldshell_mode mode, saved, alternate;
    unsigned default_clock, alternate_clock, board_count;
    const char *model;
    char advertised_model[32];
    char firmware[16], hardware[16];
    bool experimental, idle_supported, restart_supported;
} goldshell_mode_reading;

goldshell_mode_reading goldshell_mode_inspect(const char *ip);
goldshell_mode_reading goldshell_mode_authorize(const char *ip, const char *password,
    const goldshell_mode_reading *expected, bool (*allowed)(void));
goldshell_mode_reading goldshell_mode_change(const char *ip, const char *password,
    const goldshell_mode_reading *expected, goldshell_mode desired, bool (*allowed)(void));
goldshell_mode_reading goldshell_restart_inspect(const char *ip);
// Acknowledgment means the endpoint accepted the request. Confirm restart separately.
goldshell_mode_status goldshell_restart(const char *ip, const char *password,
    const goldshell_mode_reading *expected, bool (*allowed)(void));
#endif
