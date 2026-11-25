// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_ESP_LOG_H
#define DISPATCH_LINUX_ESP_LOG_H

#include <stdio.h>

#define ESP_LOGI(tag, ...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) do { fprintf(stderr, "%s: ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define ESP_LOGE ESP_LOGW

#endif
