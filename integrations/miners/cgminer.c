// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "cgminer.h"
#include "integration_profile.h"

#include "esp_timer.h"
#include "json_record.h"
#include "local_tcp.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>

static char response[JSON_RECORD_BYTES + 1];
static json_record doc;

static bool exchange(int fd, int64_t deadline, const char *request)
{
    if (!local_tcp_send(fd, request, strlen(request), deadline)) {
        return false;
    }
    size_t used = 0;
    while (used < sizeof(response)) {
        int n = local_tcp_read(fd, response + used, sizeof(response) - used, deadline);
        if (n < 0) {
            return false;
        }
        if (!n) {
            response[used] = 0;
            return used != 0;
        }
        const char *terminator = memchr(response + used, 0, n);
        if (terminator) {
            return terminator == response + used + n - 1;
        }
        used += n;
    }
    return false;
}

cgminer_sample cgminer_summary(const char *ip)
{
    cgminer_sample sample = {0};
    const int64_t deadline = esp_timer_get_time() + 1500000;
    int fd = local_tcp_open(ip, 4028, deadline);
    if (fd < 0) {
        return sample;
    }
    bool received = exchange(fd, deadline, "{\"command\":\"summary\"}");
    close(fd);
    if (!received || !json_parse(&doc, response)) {
        return sample;
    }
    unsigned status = json_first(&doc, json_field(&doc, 1, "STATUS"));
    unsigned summary = json_first(&doc, json_field(&doc, 1, "SUMMARY"));
    unsigned code = json_field(&doc, status, "STATUS");
    const bool success = json_equals(&doc, code, "\"S\"");
    sample.responding = success || json_equals(&doc, code, "\"E\"") ||
        json_equals(&doc, code, "\"W\"") || json_equals(&doc, code, "\"I\"") ||
        json_equals(&doc, code, "\"F\"");
    if (!success || !summary) {
        return sample;
    }
    sample.summary_available = true;
    const profile_cgminer *mapping = integration_profile_get(PROFILE_CGMINER);
    const struct {
        const char *field;
        uint16_t seconds;
    } rates[] = {{mapping->rate5, 5}, {mapping->rate20, 20}, {mapping->average, 0}};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(*rates); ++i) {
        double rate;
        if (json_number(&doc, json_field(&doc, summary, rates[i].field), &rate) &&
            rate >= 0 && rate <= 1e15) {
            sample.mhs = rate;
            sample.rate_window_seconds = rates[i].seconds;
            sample.activity_known = true;
            break;
        }
    }
    double count;
    if (json_number(&doc, json_field(&doc, summary, mapping->accepted), &count) &&
        count >= 0 && count < 9007199254740992.0 && count == (double)(uint64_t)count) {
        sample.accepted_known = true;
        sample.accepted = (uint64_t)count;
    }
    if (json_number(&doc, json_field(&doc, summary, mapping->elapsed), &count) &&
        count >= 0 && count <= UINT32_MAX && count == (double)(uint32_t)count) {
        sample.elapsed_known = true;
        sample.elapsed_seconds = (uint32_t)count;
    }
    return sample;
}

miner_destination cgminer_decode_destination(const char *data, const char *host, uint16_t port)
{
    if (!json_parse(&doc, data)) return DESTINATION_UNKNOWN;
    const unsigned status = json_first(&doc, json_field(&doc, 1, "STATUS"));
    if (!json_equals(&doc, json_field(&doc, status, "STATUS"), "\"S\"")) return DESTINATION_UNKNOWN;
    const unsigned pools = json_field(&doc, 1, "POOLS");
    char expected[74];
    const int length = snprintf(expected, sizeof(expected), "%s:%u", host, port);
    if (length < 1 || (size_t)length >= sizeof(expected)) return DESTINATION_UNKNOWN;
    unsigned matching = 0, other = 0;
    for (unsigned pool = json_first(&doc, pools); pool && pool < doc.tokens[pools].next; pool = doc.tokens[pool].next) {
        if (!json_equals(&doc, json_field(&doc, pool, "Stratum Active"), "true")) continue;
        const unsigned url = json_field(&doc, pool, "URL");
        if (!url) return DESTINATION_UNKNOWN;
        const char *start = data + doc.tokens[url].from;
        const char *end = data + doc.tokens[url].to;
        if (end - start < 3 || *start != '"' || end[-1] != '"') return DESTINATION_UNKNOWN;
        ++start;
        --end;
        static const char prefix[] = "stratum+tcp://";
        if (end - start >= (int)(sizeof(prefix) - 1) && !strncasecmp(start, prefix, sizeof(prefix) - 1)) {
            start += sizeof(prefix) - 1;
        }
        if (end - start == length && !strncasecmp(start, expected, length)) ++matching;
        else ++other;
    }
    if (matching == 1 && !other) return DESTINATION_MATCH;
    return !matching && other ? DESTINATION_CHANGED : DESTINATION_UNKNOWN;
}

miner_destination cgminer_destination(const char *ip, const char *host, uint16_t port)
{
    const int64_t deadline = esp_timer_get_time() + 850000;
    int fd = local_tcp_open(ip, 4028, deadline);
    if (fd < 0) return DESTINATION_UNKNOWN;
    bool received = exchange(fd, deadline, "{\"command\":\"pools\"}");
    close(fd);
    return received ? cgminer_decode_destination(response, host, port) : DESTINATION_UNKNOWN;
}

cgminer_board_sample cgminer_boards(const char *ip, unsigned expected_boards)
{
    enum { MAX_BOARDS = 4 };
    const cgminer_board_sample unavailable = {0};
    if (expected_boards > MAX_BOARDS) return unavailable;
    const int64_t deadline = esp_timer_get_time() + 850000;
    const int fd = local_tcp_open(ip, 4028, deadline);
    if (fd < 0) return unavailable;
    const bool received = exchange(fd, deadline, "{\"command\":\"devs\"}");
    close(fd);
    if (!received || !json_parse(&doc, response)) return unavailable;
    const unsigned status = json_first(&doc, json_field(&doc, 1, "STATUS"));
    if (!json_equals(&doc, json_field(&doc, status, "STATUS"), "\"S\"")) return unavailable;
    const unsigned boards = json_field(&doc, 1, "DEVS");
    cgminer_board_sample sample = {0};
    unsigned board_ids = 0;
    const unsigned limit = expected_boards ? expected_boards : MAX_BOARDS;
    for (unsigned board = json_first(&doc, boards); board && board < doc.tokens[boards].next;
         board = doc.tokens[board].next) {
        double value, id;
        const unsigned enabled = json_field(&doc, board, "Enabled");
        if ((enabled && !json_equals(&doc, enabled, "\"Y\"")) ||
            !json_equals(&doc, json_field(&doc, board, "Status"), "\"Alive\"") ||
            !json_number(&doc, json_field(&doc, board, "ID"), &id) ||
            id < 0 || id >= limit || id != (unsigned)id || (board_ids & (1u << (unsigned)id)) ||
            !json_number(&doc, json_field(&doc, board, "clock"), &value) ||
            value < 1 || value > 10000 || value != (unsigned)value || (sample.clock && sample.clock != (unsigned)value))
            return unavailable;
        sample.clock = (unsigned)value;
        ++sample.count;
        board_ids |= 1u << (unsigned)id;
        if (!expected_boards) {
            double rate;
            if ((!json_number(&doc, json_field(&doc, board, "MHS 20s"), &rate) &&
                 !json_number(&doc, json_field(&doc, board, "MHS 5s"), &rate)) ||
                rate <= 0 || rate > 1e9) return unavailable;
            sample.mhs += rate;
        }
    }
    const unsigned count = expected_boards ? expected_boards : sample.count;
    return board_ids == (1u << count) - 1 ? sample : unavailable;
}
