// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_TEST_PARTITION_H
#define DISPATCH_TEST_PARTITION_H
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
enum { ESP_PARTITION_TYPE_DATA = 1 };
typedef struct {
    size_t size, erase_size;
    bool encrypted, readonly;
    unsigned index;
} esp_partition_t;
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_partition_register_external(void *chip, size_t address, size_t size, const char *label,
    int type, int subtype, const esp_partition_t **out);
esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *data, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *part, size_t offset, const void *data, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *part, size_t offset, size_t size);
#endif
