// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct esp_tls esp_tls_t;
typedef struct {
    const char *common_name;
    const unsigned char *cacert_buf;
    unsigned cacert_bytes;
    int timeout_ms;
    bool non_block, skip_common_name;
} esp_tls_cfg_t;

enum { ESP_OK, ESP_TLS_ERR_SSL_WANT_READ = -2, ESP_TLS_ERR_SSL_WANT_WRITE = -3 };
esp_tls_t *esp_tls_init(void);
int esp_tls_conn_destroy(esp_tls_t *tls);
int esp_tls_conn_new_async(const char *ip, int length, int port, const esp_tls_cfg_t *config, esp_tls_t *tls);
int esp_tls_get_conn_sockfd(esp_tls_t *tls, int *fd);
ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *bytes, size_t size);
ssize_t esp_tls_conn_read(esp_tls_t *tls, void *bytes, size_t size);
