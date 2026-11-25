// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LINUX_ESP_HTTP_SERVER_H
#define DISPATCH_LINUX_ESP_HTTP_SERVER_H

#include "esp_err.h"
#include <stddef.h>
#include <sys/types.h>

typedef struct http_request http_request;
typedef struct {
    const char *uri;
    size_t content_len;
    int method;
    http_request *connection;
} httpd_req_t;
typedef struct http_server *httpd_handle_t;
typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *);
} httpd_uri_t;
typedef struct {
    int max_open_sockets, max_uri_handlers, lru_purge_enable;
    int recv_wait_timeout, send_wait_timeout, stack_size;
} httpd_config_t;

enum { HTTP_GET = 1, HTTP_POST, HTTP_DELETE };
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTPD_RESP_USE_STRLEN (-1)

esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config);
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_send(httpd_req_t *request, const char *data, ssize_t bytes);
esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *data);
esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *data, ssize_t bytes);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *name, char *value, size_t capacity);
size_t httpd_req_get_url_query_len(httpd_req_t *request);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *value, size_t capacity);
esp_err_t httpd_query_key_value(const char *query, const char *key, char *value, size_t capacity);
int httpd_req_recv(httpd_req_t *request, char *data, size_t bytes);

#endif
