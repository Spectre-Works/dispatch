// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_ESP_TLS_H
#define DISPATCH_LINUX_ESP_TLS_H

#include "esp_err.h"
#include <stddef.h>

typedef struct esp_tls esp_tls_t;
typedef struct {
    const char *common_name;
    const unsigned char *cacert_buf;
    size_t cacert_bytes;
    unsigned timeout_ms;
    int non_block;
} esp_tls_cfg_t;
enum { ESP_TLS_ERR_SSL_WANT_READ = -2, ESP_TLS_ERR_SSL_WANT_WRITE = -3 };

esp_tls_t *esp_tls_init(void);
int esp_tls_conn_new_async(const char *ip, int length, int port, const esp_tls_cfg_t *config, esp_tls_t *tls);
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *tls, int *fd);
int esp_tls_conn_read(esp_tls_t *tls, void *bytes, size_t size);
int esp_tls_conn_write(esp_tls_t *tls, const void *bytes, size_t size);
void esp_tls_conn_destroy(esp_tls_t *tls);

#endif
