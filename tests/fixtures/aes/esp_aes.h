// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_TEST_ESP_AES_H
#define DISPATCH_TEST_ESP_AES_H

#include <stddef.h>
enum { ESP_AES_ENCRYPT = 1 };
typedef struct { unsigned initialized; } esp_aes_context;
void esp_aes_init(esp_aes_context *ctx);
void esp_aes_free(esp_aes_context *ctx);
int esp_aes_setkey(esp_aes_context *ctx, const unsigned char *key, unsigned keybits);
int esp_aes_crypt_cbc(esp_aes_context *ctx, int mode, size_t length, unsigned char iv[16],
                     const unsigned char *input, unsigned char *output);
#endif
