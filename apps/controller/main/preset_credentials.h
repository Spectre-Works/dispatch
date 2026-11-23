// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_PRESET_CREDENTIALS_H
#define DISPATCH_PRESET_CREDENTIALS_H

#include "goldshell_mode.h"
#include "nvs.h"

// Called by the installation owner under its lock. The binding includes the slot and paired equipment.
void preset_credentials_start(nvs_handle_t store);
bool preset_credentials_persistent(void);
bool preset_credentials_available(unsigned slot);
esp_err_t preset_credentials_store(unsigned slot, const char *binding, const char *password);
bool preset_credentials_read(unsigned slot, const char *binding, char password[GOLDSHELL_PASSWORD_BYTES]);
esp_err_t preset_credentials_forget(unsigned slot);

#endif
