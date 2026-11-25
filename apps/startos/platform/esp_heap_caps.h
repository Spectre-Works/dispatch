// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_ESP_HEAP_CAPS_H
#define DISPATCH_LINUX_ESP_HEAP_CAPS_H

#include <stddef.h>

enum { MALLOC_CAP_INTERNAL = 0, MALLOC_CAP_8BIT = 0 };
static inline size_t heap_caps_get_free_size(unsigned caps) { return 0; }
static inline size_t heap_caps_get_minimum_free_size(unsigned caps) { return 0; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { return 0; }

#endif
