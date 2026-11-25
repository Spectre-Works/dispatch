// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_FREERTOS_SEMPHR_H
#define DISPATCH_LINUX_FREERTOS_SEMPHR_H

#include "FreeRTOS.h"

typedef pthread_mutex_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void xSemaphoreTake(SemaphoreHandle_t mutex, int wait);
void xSemaphoreGive(SemaphoreHandle_t mutex);

#endif
