// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_CGMINER_H
#define DISPATCH_CGMINER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool responding, summary_available, activity_known, accepted_known, elapsed_known;
    uint16_t rate_window_seconds; // Zero indicates the average since the miner started.
    uint32_t elapsed_seconds; // Mining process runtime reported by the device.
    double mhs;
    uint64_t accepted;
} cgminer_sample;

typedef enum { DESTINATION_UNKNOWN, DESTINATION_MATCH, DESTINATION_CHANGED } miner_destination;

miner_destination cgminer_destination(const char *ip, const char *host, uint16_t port);
miner_destination cgminer_decode_destination(const char *body, const char *host, uint16_t port);
cgminer_sample cgminer_summary(const char *ip);
typedef struct {
    unsigned clock, count;
    double mhs;
} cgminer_board_sample;

// A zero expected count requires recent hashrate from every reported board.
cgminer_board_sample cgminer_boards(const char *ip, unsigned expected_boards);
#endif
