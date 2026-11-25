// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_NVS_H
#define DISPATCH_LINUX_NVS_H

#include "esp_err.h"
#include <stddef.h>

typedef const char *nvs_handle_t;
enum { NVS_READWRITE };

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *data, size_t *size);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *data);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);

#endif
