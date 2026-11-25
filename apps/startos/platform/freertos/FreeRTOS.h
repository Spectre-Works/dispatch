// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_FREERTOS_FREERTOS_H
#define DISPATCH_LINUX_FREERTOS_FREERTOS_H

#include <pthread.h>
#include <stdint.h>

#define pdMS_TO_TICKS(ms) (ms)
enum { pdPASS = 1, portMAX_DELAY = -1 };

#endif
