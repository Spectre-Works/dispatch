// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_PLATFORM_H
#define DISPATCH_LINUX_PLATFORM_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

bool preview_storage_open(const char *directory);
esp_err_t preview_file_read(const char *name, void *data, size_t *bytes);
esp_err_t preview_file_write(const char *name, const void *data, size_t bytes);
bool preview_http_port(unsigned port);
int preview_http_run(void);

#endif
