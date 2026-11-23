// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "mining_path.h"
#include "cgminer.h"
#include "integration_profile.h"
#include "local_tcp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char fixture[12500], sent[720];
static size_t cursor;

int64_t esp_timer_get_time(void) { return 100; }

int local_tcp_open(const char *ip, unsigned port, int64_t deadline)
{
    assert(!strcmp(ip, "192.168.1.20") && (port == 8332 || port == 7152 || port == 4028) && deadline > 100);
    cursor = 0;
    return 99;
}

bool local_tcp_send(int fd, const char *request, size_t length, int64_t deadline)
{
    assert(fd == 99 && length < sizeof(sent) && deadline > 100);
    memcpy(sent, request, length);
    sent[length] = 0;
    return true;
}

int local_tcp_read(int fd, char *bytes, size_t capacity, int64_t deadline)
{
    assert(fd == 99 && deadline > 100);
    size_t available = strlen(fixture) - cursor;
    if (!available) return 0;
    size_t take = available < 23 ? available : 23;
    if (take > capacity) take = capacity;
    memcpy(bytes, fixture + cursor, take);
    cursor += take;
    return take;
}

static void response(unsigned status, const char *body)
{
    snprintf(fixture, sizeof(fixture), "HTTP/1.1 %u Test\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
        status, strlen(body), body);
}

int main(void)
{
    const mining_settings set = {.node_ip="192.168.1.20", .node_host="node.local", .node_port=8332,
        .rpc_user="owner", .rpc_password="read-only", .datum_ip="192.168.1.20", .datum_port=7152};
    response(200, "{\"id\":1,\"result\":{\"chain\":\"main\",\"blocks\":990001,\"headers\":990001,\"initialblockdownload\":false}}");
    node_reading node = xbt_node_status(&set);
    assert(node.status == PATH_READY && node.blocks == 990001);
    assert(strstr(sent, "POST /") && strstr(sent, "Host: node.local:8332"));
    assert(strstr(sent, "Authorization: Basic b3duZXI6cmVhZC1vbmx5") && strstr(sent, "getblockchaininfo"));
    response(200, "{\"id\":1,\"result\":{\"chain\":\"main\",\"blocks\":990000,\"headers\":990001,\"initialblockdownload\":false}}");
    assert(xbt_node_status(&set).status == PATH_SYNCING);
    response(401, "");
    assert(xbt_node_status(&set).status == PATH_AUTH);
    response(200, "{\"id\":1,\"result\":\"0000000000000050c1e5f69672f459293be14f46e5a494e7a8c8541396f18eeb\"}");
    assert(xbt_node_identity(&set) == PATH_READY && strstr(sent, "961640"));
    response(200, "{\"id\":1,\"result\":\"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\"}");
    assert(xbt_node_identity(&set) == PATH_WRONG_CHAIN);
    response(200, "{\"id\":1,\"result\":\"a-different-hash\"}");
    assert(xbt_node_identity(&set) == PATH_INVALID);
    response(200, "{\"id\":2,\"result\":\"a-different-hash\"}");
    assert(xbt_node_identity(&set) == PATH_INVALID);

    static const char ready[] = "<h2>Decentralized Client Stats</h2><table>"
        "<tr><td>Local Shares Accepted:</td><td>9000</td></tr>"
        "<tr><td class=\"label\">Pool Shares Accepted:</td><td>38  (1 diff)</td></tr>"
        "<tr><td>Pool Shares Rejected:</td><td>4  (0 diff)</td></tr>"
        "<tr><td>Status:</td><td><svg></svg> Connected and Ready </td></tr></table>";
    response(200, ready);
    datum_reading datum = datum_gateway_status(&set);
    assert(strstr(sent, "GET /") && !strstr(sent, "Authorization"));
    assert(datum.status == PATH_READY && datum.counters_known && datum.accepted == 38 && datum.rejected == 4);
    static char large[9400];
    memset(large, ' ', sizeof(large) - 1);
    memcpy(large + 2000, ready, sizeof(ready) - 1);
    response(200, large);
    assert(datum_gateway_status(&set).accepted == 38);
    datum = datum_decode_dashboard("<h2>Decentralized Client Stats</h2><table>"
        "<td>Pool Shares Accepted:</td><td>N/A</td><td>Pool Shares Rejected:</td><td>N/A</td>"
        "<td>Status:</td><td><svg></svg> Non-Pooled Mode</td></table>");
    assert(datum.status == PATH_SOLO && !datum.counters_known);
    assert(datum_decode_dashboard("<h2>Sign in</h2>").status == PATH_INVALID);

    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"SUMMARY\":[{\"Elapsed\":0,\"MHS 5s\":0,\"Accepted\":0}]}");
    cgminer_sample summary = cgminer_summary("192.168.1.20");
    assert(!strcmp(sent, "{\"command\":\"summary\"}"));
    assert(summary.summary_available && summary.elapsed_known && summary.elapsed_seconds == 0 &&
           summary.activity_known && summary.mhs == 0 && summary.accepted_known && summary.accepted == 0);
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"SUMMARY\":[{\"Elapsed\":86412}]}");
    summary = cgminer_summary("192.168.1.20");
    assert(summary.elapsed_known && summary.elapsed_seconds == 86412);
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"SUMMARY\":[{\"Elapsed\":4294967296}]}");
    assert(!cgminer_summary("192.168.1.20").elapsed_known);
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"SUMMARY\":[{\"Elapsed\":0.25}]}");
    assert(!cgminer_summary("192.168.1.20").elapsed_known);
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"SUMMARY\":[{\"Accepted\":12}]}");
    assert(!cgminer_summary("192.168.1.20").elapsed_known);

    integration_profile custom = {.cgminer = {"Rate5", "Rate20", "Average", "Count", "RunSeconds"}};
    assert(integration_profile_activate(PROFILE_CGMINER, &custom));
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"SUMMARY\":[{\"RunSeconds\":123,\"Rate20\":1024,\"Count\":42}]}");
    summary = cgminer_summary("192.168.1.20");
    assert(summary.elapsed_known && summary.elapsed_seconds == 123 && summary.activity_known &&
           summary.mhs == 1024 && summary.rate_window_seconds == 20 && summary.accepted_known && summary.accepted == 42);

    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"POOLS\":[{\"Stratum Active\":true,"
        "\"URL\":\"stratum+tcp://datum.local:23334\"}]}");
    assert(cgminer_destination("192.168.1.20", "datum.local", 23334) == DESTINATION_MATCH);
    assert(!strcmp(sent, "{\"command\":\"pools\"}"));
    assert(cgminer_decode_destination(fixture, "another.local", 23334) == DESTINATION_CHANGED);
    assert(cgminer_decode_destination("{\"STATUS\":[{\"STATUS\":\"S\"}],\"POOLS\":[{\"Stratum Active\":false,"
        "\"URL\":\"datum.local:23334\"}]}", "datum.local", 23334) == DESTINATION_UNKNOWN);
    assert(cgminer_decode_destination("{\"STATUS\":[{\"STATUS\":\"S\"}],\"POOLS\":["
        "{\"Stratum Active\":true,\"URL\":\"datum.local:23334\"},"
        "{\"Stratum Active\":true,\"URL\":\"other.local:23334\"}]}", "datum.local", 23334) == DESTINATION_UNKNOWN);
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"DEVS\":[{\"ID\":0,\"Enabled\":\"Y\",\"Status\":\"Alive\",\"clock\":525}]}");
    assert(cgminer_boards("192.168.1.20", 1).clock == 525);
    assert(!strcmp(sent, "{\"command\":\"devs\"}"));
    assert(cgminer_boards("192.168.1.20", 2).clock == 0);
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"DEVS\":[{\"ID\":0,\"Enabled\":\"Y\",\"Status\":\"Alive\",\"clock\":650},{\"ID\":1,\"Enabled\":\"Y\",\"Status\":\"Alive\",\"clock\":650}]}");
    assert(cgminer_boards("192.168.1.20", 2).clock == 650);
    assert(cgminer_boards("192.168.1.20", 1).clock == 0);
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"DEVS\":[{\"ID\":0,\"Enabled\":\"Y\",\"Status\":\"Alive\",\"clock\":650},{\"ID\":1,\"Enabled\":\"Y\",\"Status\":\"Alive\",\"clock\":800}]}");
    assert(cgminer_boards("192.168.1.20", 2).clock == 0);
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"DEVS\":[{\"ID\":0,\"Enabled\":\"N\",\"Status\":\"Alive\",\"clock\":525}]}");
    assert(cgminer_boards("192.168.1.20", 1).clock == 0);
    // Four full records cover SC5 Pro without relying on an Enabled field.
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"DEVS\":[");
    for (unsigned board = 0; board < 4; ++board) {
        char start[24];
        snprintf(start, sizeof(start), "%s{\"ID\":%u,", board ? "," : "", board);
        strcat(fixture, start);
        for (unsigned i = 0; i < 42; ++i) {
            char field[40];
            snprintf(field, sizeof(field), "\"Sample%u\":104.509,", i);
            strcat(fixture, field);
        }
        strcat(fixture, "\"Status\":\"Alive\",\"clock\":840.0}");
    }
    strcat(fixture, "]}");
    assert(cgminer_boards("192.168.1.20", 4).clock == 840);
    assert(cgminer_boards("192.168.1.20", 3).clock == 0);
    assert(cgminer_boards("192.168.1.20", 0).clock == 0);
    strstr(fixture, "\"ID\":3")[5] = '2';
    assert(cgminer_boards("192.168.1.20", 4).clock == 0);
    strcpy(fixture, "{\"STATUS\":[{\"STATUS\":\"S\"}],\"DEVS\":["
        "{\"ID\":1,\"Status\":\"Alive\",\"clock\":750,\"MHS 20s\":7100000},"
        "{\"ID\":0,\"Status\":\"Alive\",\"clock\":750,\"MHS 5s\":6900000}]}");
    const cgminer_board_sample qualified = cgminer_boards("192.168.1.20", 0);
    assert(qualified.clock == 750 && qualified.count == 2 && qualified.mhs == 14e6);
    strstr(fixture, "\"ID\":1")[5] = '2';
    assert(cgminer_boards("192.168.1.20", 0).clock == 0);
    puts("XBT RPC, CONVOY DATUM, active miner destination and board telemetry fixtures passed");
}
