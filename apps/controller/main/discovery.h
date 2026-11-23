// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_DISCOVERY_H
#define DISPATCH_DISCOVERY_H

#include "esp_err.h"
#include "shelly.h"

enum { DISCOVERY_LIMIT = 8 };

typedef struct {
    char ip[16], identity[40];
    shelly_sample sample;
    shelly_result result;
    uint32_t observed_at;
} discovery_device;

typedef struct {
    discovery_device device[DISCOVERY_LIMIT];
    unsigned count;
    bool running, failed, limited;
} discovery_view;

esp_err_t discovery_request(void);
void discovery_read(discovery_view *view);
void discovery_tick(void);
#endif
