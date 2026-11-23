// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef PROFILE_TEST_NVS_H
#define PROFILE_TEST_NVS_H
#include "esp_err.h"
#include <stddef.h>
typedef unsigned nvs_handle_t;
enum { NVS_READWRITE };
esp_err_t nvs_open(const char *name, int flags, nvs_handle_t *handle);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *text, size_t *bytes);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *text);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);
#endif
