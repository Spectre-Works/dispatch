// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_MBEDTLS_BASE64_H
#define DISPATCH_LINUX_MBEDTLS_BASE64_H

#include <stddef.h>

int mbedtls_base64_encode(unsigned char *dst, size_t capacity, size_t *written,
                          const unsigned char *source, size_t bytes);
int mbedtls_base64_decode(unsigned char *dst, size_t capacity, size_t *written,
                          const unsigned char *source, size_t bytes);

#endif
