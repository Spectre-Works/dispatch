// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_OPERATIONS_STORE_H
#define DISPATCH_OPERATIONS_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Open once at boot. The installation lock serializes subsequent saves.
bool operations_load(void *state, size_t size);
esp_err_t operations_save(const void *state);

#endif
