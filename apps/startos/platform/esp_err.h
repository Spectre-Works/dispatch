// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_ESP_ERR_H
#define DISPATCH_LINUX_ESP_ERR_H

#include <stdlib.h>

typedef int esp_err_t;
enum {
    ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_STATE,
    ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED, ESP_ERR_NO_MEM, ESP_ERR_NVS_NOT_FOUND
};
const char *esp_err_to_name(esp_err_t error);
#define ESP_ERROR_CHECK(call) do { if ((call) != ESP_OK) abort(); } while (0)

#endif
