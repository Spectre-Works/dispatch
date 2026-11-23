// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_PROFILE_STORE_H
#define DISPATCH_PROFILE_STORE_H

#include "esp_err.h"
#include "integration_profile.h"

typedef struct {
    bool custom, saved_custom, pending, invalid;
} profile_status;

void profile_store_start(void);
profile_status profile_store_status(profile_family family);
esp_err_t profile_store_stage(char *text);
esp_err_t profile_store_reset(profile_family family);

#endif
