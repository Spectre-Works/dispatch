// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "mining_path.h"
#include "local_http.h"
#include "json_record.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static json_record doc;

static void base64(const unsigned char *p, size_t size, char *out)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    while (size) {
        const unsigned packed = ((unsigned)p[0] << 16) | (size > 1 ? (unsigned)p[1] << 8 : 0) | (size > 2 ? p[2] : 0);
        *out++ = alphabet[(packed >> 18) & 63];
        *out++ = alphabet[(packed >> 12) & 63];
        *out++ = size > 1 ? alphabet[(packed >> 6) & 63] : '=';
        *out++ = size > 2 ? alphabet[packed & 63] : '=';
        if (size <= 3) break;
        p += 3;
        size -= 3;
    }
    *out = 0;
}

static int node_rpc(const mining_settings *s, const char *payload)
{
    char pair[sizeof(s->rpc_user) + sizeof(s->rpc_password)];
    char encoded[4 * ((sizeof(pair) + 2) / 3) + 1];
    char request[530];
    int bytes = snprintf(pair, sizeof(pair), "%s:%s", s->rpc_user, s->rpc_password);
    base64((const unsigned char *)pair, bytes, encoded);
    explicit_bzero(pair, sizeof(pair));
    int length = snprintf(request, sizeof(request),
        "POST / HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nContent-Type: application/json\r\n"
        "Authorization: Basic %s\r\nContent-Length: %u\r\n\r\n%s",
        s->node_host[0] ? s->node_host : s->node_ip, s->node_port, encoded, (unsigned)strlen(payload), payload);
    explicit_bzero(encoded, sizeof(encoded));
    const int result = length > 0 && (size_t)length < sizeof(request)
        ? local_http_response(s->node_ip, s->node_port, request, length, NULL, 0, false) : 0;
    explicit_bzero(request, sizeof(request));
    return result;
}

static unsigned rpc_result(const char *response)
{
    if (!json_parse(&doc, response) ||
        !json_equals(&doc, json_field(&doc, 1, "id"), "1")) return 0;
    return json_field(&doc, 1, "result");
}

static bool number_at(unsigned object, const char *field, uint32_t *out)
{
    double n;
    if (!json_number(&doc, json_field(&doc, object, field), &n) ||
        n < 0 || n > UINT32_MAX || n != (uint32_t)n) return false;
    *out = n;
    return true;
}

node_reading xbt_decode_status(const char *response)
{
    node_reading reading = {.status = PATH_INVALID};
    const unsigned result = rpc_result(response);
    const unsigned ibd = json_field(&doc, result, "initialblockdownload");
    if (!result || !number_at(result, "blocks", &reading.blocks) ||
        !number_at(result, "headers", &reading.headers) ||
        !(json_equals(&doc, ibd, "true") || json_equals(&doc, ibd, "false"))) return reading;
    if (!json_equals(&doc, json_field(&doc, result, "chain"), "\"main\"")) {
        reading.status = PATH_WRONG_CHAIN;
    } else {
        reading.status = json_equals(&doc, ibd, "true") || reading.blocks < XBT_CHECKPOINT_HEIGHT ||
            reading.blocks < reading.headers ? PATH_SYNCING : PATH_READY;
    }
    return reading;
}

path_status xbt_decode_identity(const char *response)
{
    const unsigned result = rpc_result(response);
    if (!result) return PATH_INVALID;
    const json_token token = doc.tokens[result];
    const char *hash = response + token.from;
    if (token.to - token.from != 66 || hash[0] != '"' ||
        hash[65] != '"' || strspn(hash + 1, "0123456789abcdef") != 64) return PATH_INVALID;
    return json_equals(&doc, result, "\"0000000000000050c1e5f69672f459293be14f46e5a494e7a8c8541396f18eeb\"")
        ? PATH_READY : PATH_WRONG_CHAIN;
}

static path_status failed_status(int code)
{
    return !code ? PATH_OFFLINE : code == 401 || code == 403 ? PATH_AUTH : PATH_INVALID;
}

node_reading xbt_node_status(const mining_settings *settings)
{
    const int code = node_rpc(settings, "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"getblockchaininfo\",\"params\":[]}");
    return code == 200 ? xbt_decode_status(local_http_body()) : (node_reading){.status = failed_status(code)};
}

path_status xbt_node_identity(const mining_settings *settings)
{
    const int code = node_rpc(settings, "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"getblockhash\",\"params\":[961640]}");
    return code == 200 ? xbt_decode_identity(local_http_body()) : failed_status(code);
}

static const char *cell(const char *start, const char *label, const char **end)
{
    const char *label_at = strstr(start, label);
    if (!label_at || label_at >= *end) return NULL;
    const char *close = strstr(label_at, "</td>");
    const char *next = close ? strstr(close + 5, "<td") : NULL;
    if (!next || next >= *end || !(next = strchr(next, '>'))) return NULL;
    close = strstr(++next, "</td>");
    if (!close || close > *end) return NULL;
    *end = close;
    while (next < close && isspace((unsigned char)*next)) ++next;
    return next;
}

static bool counter(const char *start, const char *end, const char *label, uint64_t *value)
{
    const char *first = cell(start, label, &end);
    if (!first || first >= end || !isdigit((unsigned char)*first)) return false;
    uint64_t count = 0;
    while (first < end && isdigit((unsigned char)*first)) {
        unsigned digit = *first++ - '0';
        if (count > (UINT64_MAX - digit) / 10) return false;
        count = count * 10 + digit;
    }
    if (first < end && !isspace((unsigned char)*first) && *first != '(') return false;
    *value = count;
    return true;
}

datum_reading datum_decode_dashboard(const char *response)
{
    datum_reading r = {.status = PATH_INVALID};
    const char *start = strstr(response, "Decentralized Client Stats");
    const char *end = start ? strstr(start, "</table>") : NULL;
    if (!end) return r;
    const char *status_end = end;
    const char *status = cell(start, "Status:", &status_end);
    const char *accepted_end = end, *rejected_end = end;
    const char *local_end = end;
    const bool separate_local = cell(start, "Local Shares Accepted:", &local_end) != NULL;
    const char *accepted = separate_local ? "Pool Shares Accepted:" : "Shares Accepted:";
    const char *rejected = separate_local ? "Pool Shares Rejected:" : "Shares Rejected:";
    if (!status || !cell(start, accepted, &accepted_end) || !cell(start, rejected, &rejected_end)) return r;
    const char *plain = strstr(status, "</svg>");
    if (plain && plain < status_end) status = plain + 6;
    while (status < status_end && isspace((unsigned char)*status)) ++status;
    while (status_end > status && isspace((unsigned char)status_end[-1])) --status_end;
    const size_t len = status_end - status;
    r.status = len == 19 && !memcmp(status, "Connected and Ready", len) ? PATH_READY :
        len == 15 && !memcmp(status, "Non-Pooled Mode", len) ? PATH_SOLO : PATH_UNREADY;
    r.counters_known = counter(start, end, accepted, &r.accepted) && counter(start, end, rejected, &r.rejected);
    return r;
}

datum_reading datum_gateway_status(const mining_settings *settings)
{
    char request[167];
    const int length = snprintf(request, sizeof(request), "GET / HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n",
                                settings->datum_host[0] ? settings->datum_host : settings->datum_ip,
                                settings->datum_port);
    const int code = local_http_response(settings->datum_ip, settings->datum_port, request, length, NULL, 0, true);
    return code == 200 ? datum_decode_dashboard(local_http_body()) : (datum_reading){.status = failed_status(code)};
}
