// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_PSA_CRYPTO_H
#define DISPATCH_LINUX_PSA_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

enum { PSA_SUCCESS, PSA_ALG_SHA_256 };
static inline int psa_crypto_init(void) { return PSA_SUCCESS; }
int psa_hash_compute(int alg, const uint8_t *data, size_t bytes, uint8_t *digest, size_t capacity, size_t *used);

#endif
