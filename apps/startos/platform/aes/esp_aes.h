// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_AES_ESP_AES_H
#define DISPATCH_LINUX_AES_ESP_AES_H

#include <openssl/evp.h>

enum { ESP_AES_ENCRYPT = 1 };
typedef struct { EVP_CIPHER_CTX *cipher; } esp_aes_context;
void esp_aes_init(esp_aes_context *aes);
int esp_aes_setkey(esp_aes_context *aes, const unsigned char *key, unsigned bits);
int esp_aes_crypt_cbc(esp_aes_context *aes, int mode, size_t bytes, unsigned char *iv,
                      const unsigned char *input, unsigned char *output);
void esp_aes_free(esp_aes_context *aes);

#endif
