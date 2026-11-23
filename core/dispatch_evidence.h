// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_EVIDENCE_H
#define DISPATCH_EVIDENCE_H
#include <stdbool.h>
#include <stdint.h>

enum { DISPATCH_EVIDENCE_DAYS = 7, DISPATCH_PROFILE_WEIGHT = 32 };

typedef struct {
    double mean_j_per_gh;
    uint16_t samples;
} dispatch_efficiency;

enum { DISPATCH_PRESET_WINDOW_SECONDS = 600, DISPATCH_PRESET_WINDOWS = 3 };

typedef struct {
    double joules, terahashes;
    uint32_t seconds;
    float peak_watts;
} dispatch_preset_totals;

typedef struct {
    dispatch_preset_totals totals;
    float windows[DISPATCH_PRESET_WINDOWS];
    uint8_t next_window;
} dispatch_preset_evidence;

typedef enum {
    DISPATCH_PRESET_LEARNING, DISPATCH_PRESET_SIMILAR, DISPATCH_PRESET_DEFAULT, DISPATCH_PRESET_REDUCED,
} dispatch_preset_comparison;

typedef enum { DISPATCH_WORK_IDLE, DISPATCH_WORK_PRESENT, DISPATCH_WORK_MISSING } dispatch_work;
typedef enum { DISPATCH_WORK_UNCHANGED, DISPATCH_WORK_LOST, DISPATCH_WORK_RESTORED } dispatch_work_change;
typedef struct {
    uint32_t since;
    dispatch_work evidence;
    bool active;
} dispatch_work_monitor;

typedef struct {
    double mean_mhs, min_mhs, max_mhs;
    float mean_watts, min_watts, max_watts;
    uint16_t hash_samples, power_samples, startups;
    uint32_t startup_seconds;
} dispatch_profile;

typedef struct {
    uint64_t energy_mwh, cost_nano_cents;
    uint32_t day, powered_seconds, interruptions;
    bool metered, gap_accepted;
} dispatch_daily_evidence;

typedef struct {
    uint16_t peak, running;
} dispatch_power_test_result;

typedef struct {
    uint32_t since, sum;
    uint16_t peak, low, high, count;
} dispatch_power_test_samples;

// Samples arrive from existing outlet polls, with one observation per timestamp.
// Returns a result only after a stable operating window.
bool dispatch_power_test_sample(dispatch_power_test_samples *test, uint32_t now,
    uint16_t watts, bool operating, unsigned stable_seconds, dispatch_power_test_result *result);

dispatch_daily_evidence *dispatch_evidence_day(dispatch_daily_evidence days[DISPATCH_EVIDENCE_DAYS], uint32_t day);
void dispatch_profile_sample(dispatch_profile *profile, double mhs, bool power_known, double watts);
void dispatch_profile_start(dispatch_profile *profile, uint32_t seconds);
bool dispatch_efficiency_sample(dispatch_efficiency *efficiency, double watts, double mhs);
bool dispatch_preset_sample(dispatch_preset_totals *totals, uint32_t seconds, double watts, double mhs, double peak_watts);
void dispatch_preset_merge(dispatch_preset_totals *totals, const dispatch_preset_totals *period);
void dispatch_preset_record_window(dispatch_preset_evidence *evidence, const dispatch_preset_totals *window);
double dispatch_preset_efficiency(const dispatch_preset_totals *totals);
unsigned dispatch_preset_window_count(const dispatch_preset_evidence *evidence);
dispatch_preset_comparison dispatch_preset_compare(const dispatch_preset_evidence *normal, const dispatch_preset_evidence *reduced);
dispatch_work_change dispatch_work_update(dispatch_work_monitor *monitor, dispatch_work evidence, uint32_t now);

#endif
