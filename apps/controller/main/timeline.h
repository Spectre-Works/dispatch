// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_TIMELINE_H
#define DISPATCH_TIMELINE_H

#include "dispatch_policy.h"

enum { TIMELINE_SECONDS = 300, TIMELINE_BUCKETS = 576, TIMELINE_PAGE = 72 };
enum { TIMELINE_UNKNOWN_WATTS = UINT16_MAX, TIMELINE_UNKNOWN_GRID = INT16_MIN };
enum { TIMELINE_PRESET_MASK = 7, TIMELINE_OUTLET_OFF = 8, TIMELINE_OUTLET_ON = 16,
       TIMELINE_METERED = 32, TIMELINE_CATCHUP = 64, TIMELINE_PREVIEW = 128 };
enum { TIMELINE_BRIDGE = 1, TIMELINE_PARTIAL = 2, TIMELINE_BUDGET = 4,
       TIMELINE_ACCOUNTING_GAP = 8, TIMELINE_PRODUCTIVE = 16, TIMELINE_SAMPLE_TAG = 0x5400 };
enum { TIMELINE_GRID_HOLD = DISPATCH_DAYLIGHT_EVIDENCE + 1, TIMELINE_GENERATION_HOLD };
enum { TIMELINE_REASON_MASK = 0x3f, TIMELINE_PAID_WINDOW = 0x40, TIMELINE_PAID_GAP = 0x80 };
_Static_assert((TIMELINE_PRODUCTIVE << DISPATCH_LIMIT) <= 0x100, "Activity flag byte capacity");

typedef struct {
    uint32_t paid_mwh;
    uint16_t identity, watts, cost_centicents;
    uint8_t reason, status;
} timeline_miner;

typedef struct {
    uint32_t spent_millicents, allowance_millicents;
    uint16_t solar_decawatts;
    int16_t grid_decawatts;
    uint16_t flags, crc;
    timeline_miner miner[DISPATCH_LIMIT];
} timeline_sample;

typedef struct {
    uint32_t epoch, sequence;
    uint16_t identity;
    uint8_t slot, kind, reason, action;
    uint16_t crc;
} timeline_event;

// Writers run under the installation lock. The web reader never holds that lock while sending.
void timeline_start(void);
bool timeline_available(void);
bool timeline_sample_read(uint32_t bucket, timeline_sample *sample);
bool timeline_event_read(uint32_t sequence, timeline_event *event);
uint32_t timeline_event_cursor(void);
void timeline_sample_write(uint32_t bucket, timeline_sample *sample);
void timeline_queue_event(timeline_event event);
void timeline_flush_events(void);

#endif
