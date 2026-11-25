// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_FREERTOS_TASK_H
#define DISPATCH_LINUX_FREERTOS_TASK_H

#include "FreeRTOS.h"

int xTaskCreate(void (*worker)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle);
void vTaskDelay(unsigned ms);
static inline unsigned uxTaskGetStackHighWaterMark(void *task) { return 1; }

#endif
