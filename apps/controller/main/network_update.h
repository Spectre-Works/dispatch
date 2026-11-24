// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_NETWORK_UPDATE_H
#define DISPATCH_NETWORK_UPDATE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    UPDATE_IDLE, UPDATE_CHECK_QUEUED, UPDATE_CHECKING, UPDATE_INSTALL_QUEUED,
    UPDATE_DOWNLOADING, UPDATE_STAGED, UPDATE_RESTARTING
} update_phase;

typedef enum {
    UPDATE_RESULT_NONE, UPDATE_RESULT_CURRENT, UPDATE_RESULT_AVAILABLE,
    UPDATE_RESULT_CONNECTION, UPDATE_RESULT_MANIFEST, UPDATE_RESULT_IMAGE,
    UPDATE_RESULT_STORAGE, UPDATE_RESULT_MEMORY, UPDATE_RESULT_ROLLBACK
} update_result;

typedef struct {
    char installed[32], offered[32];
    bool supported, enabled, unread;
    update_phase phase;
    update_result result;
    uint32_t received, total;
} network_update_view;

void network_update_start(void);
void network_update_confirm_boot(bool healthy);
void network_update_read(network_update_view *view);
esp_err_t network_update_enable(bool enabled);
esp_err_t network_update_check(void);
esp_err_t network_update_seen(const char *version);
esp_err_t network_update_install(const char *version);
void network_update_tick(void);
bool network_update_staged(void);
void network_update_restart(bool saved);
bool network_update_active(void);

#endif
