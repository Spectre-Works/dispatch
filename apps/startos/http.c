// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "esp_http_server.h"
#include "installation.h"
#include "platform.h"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

enum { ROUTE_LIMIT = 16, REQUEST_BYTES = 8192 };
struct http_server {
    struct event_base *loop;
    struct evhttp *http;
    httpd_uri_t routes[ROUTE_LIMIT];
    unsigned count;
};
struct http_request {
    struct evhttp_request *message;
    const char *query;
    const char *reason;
    int status;
    bool sent, streaming;
};

static struct http_server listener;
static unsigned listen_port;

bool preview_http_port(unsigned port)
{
    listen_port = port;
    return port > 0;
}

static void health(struct evhttp_request *message)
{
    const bool healthy = installation_boot_healthy();
    struct evbuffer *body = evbuffer_new();
    if (body) evbuffer_add_printf(body, "%s\n", healthy ? "ready" : "unavailable");
    evhttp_add_header(evhttp_request_get_output_headers(message), "Content-Type", "text/plain");
    evhttp_add_header(evhttp_request_get_output_headers(message), "Cache-Control", "no-store");
    evhttp_send_reply(message, healthy ? 200 : 503, NULL, body);
    if (body) evbuffer_free(body);
}

static void dispatch(struct evhttp_request *message, void *server)
{
    const struct http_server *http = server;
    const struct evhttp_uri *uri = evhttp_request_get_evhttp_uri(message);
    const char *path = uri ? evhttp_uri_get_path(uri) : NULL;
    const enum evhttp_cmd_type command = evhttp_request_get_command(message);
    const int method = command == EVHTTP_REQ_GET ? HTTP_GET :
        command == EVHTTP_REQ_POST ? HTTP_POST : command == EVHTTP_REQ_DELETE ? HTTP_DELETE : 0;
    if (!path || !method) {
        evhttp_send_error(message, 400, NULL);
        return;
    }
    if (method == HTTP_GET && !strcmp(path, "/healthz")) {
        health(message);
        return;
    }
    for (unsigned i = 0; i < http->count; ++i) {
        const httpd_uri_t *route = &http->routes[i];
        if (strcmp(path, route->uri) || method != route->method) continue;
        struct http_request connection = {
            .message = message, .query = evhttp_uri_get_query(uri), .status = 200, .reason = "OK",
        };
        httpd_req_t request = {.uri = path, .method = method, .connection = &connection,
            .content_len = evbuffer_get_length(evhttp_request_get_input_buffer(message))};
        route->handler(&request);
        if (connection.streaming) evhttp_send_reply_end(message);
        else if (!connection.sent) evhttp_send_error(message, 500, NULL);
        return;
    }
    evhttp_send_error(message, 404, NULL);
}

esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config)
{
    listener.loop = event_base_new();
    listener.http = listener.loop ? evhttp_new(listener.loop) : NULL;
    if (!listener.http) return ESP_FAIL;
    evhttp_set_timeout(listener.http, config->recv_wait_timeout);
    evhttp_set_max_headers_size(listener.http, REQUEST_BYTES);
    evhttp_set_max_body_size(listener.http, REQUEST_BYTES);
    evhttp_set_allowed_methods(listener.http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_DELETE);
    evhttp_set_gencb(listener.http, dispatch, &listener);
    if (evhttp_bind_socket(listener.http, "0.0.0.0", listen_port) < 0) return ESP_FAIL;
    *server = &listener;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route)
{
    if (server->count == ROUTE_LIMIT) return ESP_FAIL;
    server->routes[server->count++] = *route;
    return ESP_OK;
}

static void stop(evutil_socket_t sig, short events, void *arg)
{
    event_base_loopbreak(arg);
}

int preview_http_run(void)
{
    struct event *term = evsignal_new(listener.loop, SIGTERM, stop, listener.loop);
    struct event *interrupt = evsignal_new(listener.loop, SIGINT, stop, listener.loop);
    const bool ready = term && interrupt && event_add(term, NULL) == 0 && event_add(interrupt, NULL) == 0;
    const int result = ready ? event_base_dispatch(listener.loop) : -1;
    if (term) event_free(term);
    if (interrupt) event_free(interrupt);
    evhttp_free(listener.http);
    event_base_free(listener.loop);
    return result < 0;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value)
{
    return evhttp_add_header(evhttp_request_get_output_headers(request->connection->message), name, value) == 0
        ? ESP_OK : ESP_FAIL;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    char *reason;
    const long code = strtol(status, &reason, 10);
    if (code < 100 || code > 599 || *reason != ' ') return ESP_ERR_INVALID_ARG;
    request->connection->status = code;
    request->connection->reason = reason + 1;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    return httpd_resp_set_hdr(request, "Content-Type", type);
}

static struct evbuffer *response_buffer(const char *data, size_t bytes)
{
    struct evbuffer *body = evbuffer_new();
    if (body && evbuffer_add(body, data, bytes)) {
        evbuffer_free(body);
        return NULL;
    }
    return body;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *data, ssize_t bytes)
{
    struct http_request *connection = request->connection;
    struct evbuffer *body = response_buffer(data, bytes < 0 ? strlen(data) : (size_t)bytes);
    if (!body) return ESP_FAIL;
    connection->sent = true;
    evhttp_send_reply(connection->message, connection->status, connection->reason, body);
    evbuffer_free(body);
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *data)
{
    return httpd_resp_send(request, data, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *data, ssize_t bytes)
{
    struct http_request *connection = request->connection;
    if (!connection->streaming) {
        evhttp_send_reply_start(connection->message, connection->status, connection->reason);
        connection->streaming = true;
    }
    if (!bytes) {
        evhttp_send_reply_end(connection->message);
        connection->streaming = false;
        connection->sent = true;
        return ESP_OK;
    }
    struct evbuffer *body = response_buffer(data, bytes < 0 ? strlen(data) : (size_t)bytes);
    if (!body) return ESP_FAIL;
    evhttp_send_reply_chunk(connection->message, body);
    evbuffer_free(body);
    return ESP_OK;
}

static esp_err_t copy_field(const char *source, size_t bytes, char *value, size_t capacity)
{
    if (!source) return ESP_ERR_NOT_FOUND;
    if (bytes >= capacity) return ESP_ERR_INVALID_SIZE;
    memcpy(value, source, bytes);
    value[bytes] = 0;
    return ESP_OK;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *name, char *value, size_t capacity)
{
    const char *header = evhttp_find_header(evhttp_request_get_input_headers(request->connection->message), name);
    return copy_field(header, header ? strlen(header) : 0, value, capacity);
}

size_t httpd_req_get_url_query_len(httpd_req_t *request)
{
    return request->connection->query ? strlen(request->connection->query) : 0;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *value, size_t capacity)
{
    return copy_field(request->connection->query, httpd_req_get_url_query_len(request), value, capacity);
}

esp_err_t httpd_query_key_value(const char *query, const char *key, char *value, size_t capacity)
{
    const size_t length = strlen(key);
    while (*query) {
        const size_t bytes = strcspn(query, "&");
        if (bytes > length && query[length] == '=' && !strncmp(query, key, length))
            return copy_field(query + length + 1, bytes - length - 1, value, capacity);
        query += bytes;
        if (*query) ++query;
    }
    return ESP_ERR_NOT_FOUND;
}

int httpd_req_recv(httpd_req_t *request, char *data, size_t bytes)
{
    return evbuffer_remove(evhttp_request_get_input_buffer(request->connection->message), data, bytes);
}
