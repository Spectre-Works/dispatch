// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "local_http.h"

#include "esp_timer.h"
#include "http_parser.h"
#include "local_tcp.h"
#include <string.h>
#include <unistd.h>

// Reused serially on the installation worker. A gateway dashboard is larger
// than an RPC record. Its Decentralized Client Stats appear near the top.
static char body[8193];

typedef struct {
    size_t used;
    bool complete, prefix_only;
} response_state;

static int response_body(http_parser *parser, const char *data, size_t length)
{
    response_state *state = parser->data;
    const size_t available = sizeof(body) - state->used - 1;
    if (length > available && !state->prefix_only) return -1;
    const size_t take = length < available ? length : available;
    if (memchr(data, 0, take)) return -1;
    memcpy(body + state->used, data, take);
    state->used += take;
    body[state->used] = 0;
    if (state->prefix_only && state->used == sizeof(body) - 1) http_parser_pause(parser, 1);
    return 0;
}

static int response_done(http_parser *parser)
{
    ((response_state *)parser->data)->complete = true;
    http_parser_pause(parser, 1);
    return 0;
}

char *local_http_body(void)
{
    return body;
}

const json_record *local_http_json(void)
{
    return json_parse_shared(body);
}

int local_http_response(const char *ip, unsigned port, const char *header, size_t header_bytes,
                        const char *payload, size_t payload_bytes, bool prefix)
{
    return local_http_response_until(ip, port, header, header_bytes, payload, payload_bytes,
                                     prefix, esp_timer_get_time() + 850000);
}

int local_http_response_until(const char *ip, unsigned port, const char *header, size_t header_bytes,
                              const char *payload, size_t payload_bytes, bool prefix, int64_t deadline)
{
    int fd = local_tcp_open(ip, port, deadline);
    if (fd < 0) return 0;
    response_state state = {.prefix_only = prefix};
    http_parser parser;
    http_parser_init(&parser, HTTP_RESPONSE);
    parser.data = &state;
    static const http_parser_settings callbacks = {.on_body = response_body, .on_message_complete = response_done};
    const bool sent = local_tcp_send(fd, header, header_bytes, deadline) &&
        (!payload_bytes || local_tcp_send(fd, payload, payload_bytes, deadline));
    body[0] = 0;
    size_t total = 0;
    if (sent) {
        while (total < sizeof(body) + 2048) {
            char packet[512];
            int bytes = local_tcp_read(fd, packet, sizeof(packet), deadline);
            if (bytes < 0) break;
            total += bytes;
            size_t parsed = http_parser_execute(&parser, &callbacks, packet, bytes);
            if (!bytes || parsed != (size_t)bytes || HTTP_PARSER_ERRNO(&parser) != HPE_OK) break;
        }
    }
    close(fd);
    const bool usable_prefix = prefix && state.used == sizeof(body) - 1;
    return (state.complete || usable_prefix) && HTTP_PARSER_ERRNO(&parser) == HPE_PAUSED ? parser.status_code : 0;
}
