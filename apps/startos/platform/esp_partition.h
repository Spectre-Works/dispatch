// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_ESP_PARTITION_H
#define DISPATCH_LINUX_ESP_PARTITION_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

enum { ESP_PARTITION_TYPE_DATA = 1 };
typedef struct {
    const char *name;
    size_t size, erase_size;
    int fd;
    bool encrypted, readonly;
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(unsigned type, unsigned subtype, const char *name);
void esp_partition_register_external(void *chip, size_t offset, size_t size, const char *name,
                                     unsigned type, unsigned subtype, const esp_partition_t **part);
esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *data, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *part, size_t offset, const void *data, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *part, size_t offset, size_t size);

#endif
