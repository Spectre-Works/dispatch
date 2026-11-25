// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_ESP_CRC_H
#define DISPATCH_LINUX_ESP_CRC_H

#include <stddef.h>
#include <stdint.h>

uint16_t esp_crc16_le(uint16_t crc, const uint8_t *data, size_t size);
uint32_t esp_crc32_le(uint32_t crc, const uint8_t *data, size_t size);

#endif
