// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_POLICY_H
#define DISPATCH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum { DISPATCH_LIMIT = 8, DISPATCH_METER_CHANNEL_WATTS = 2400 };
enum { DISPATCH_MIN_ON = 120, DISPATCH_MIN_OFF = 60, DISPATCH_STAGGER = 20 };
enum { DISPATCH_RATE_WINDOWS = 2, DISPATCH_RATE_MAX = 1000000, DISPATCH_PRORATE_SECONDS = 300 };

typedef enum {
    DISPATCH_UNOBSERVED,
    DISPATCH_PAUSED,
    DISPATCH_CLOCK,
    DISPATCH_WINDOW_CLOSED,
    DISPATCH_BUDGET_UNKNOWN,
    DISPATCH_BUDGET_USED,
    DISPATCH_WINDOW_OPEN,
    DISPATCH_DWELL,
    DISPATCH_START_PENDING,
    DISPATCH_COMMAND_HOLD,
    DISPATCH_QUIET_HOURS,
    DISPATCH_QUIET_DWELL,
    DISPATCH_MANUAL_HOLD, DISPATCH_RECOVERY_HOLD, DISPATCH_RECOVERY,
    DISPATCH_SURPLUS_STALE, DISPATCH_SURPLUS_WAIT, DISPATCH_SURPLUS_POWER,
    DISPATCH_SURPLUS_START, DISPATCH_SURPLUS_STOP, DISPATCH_CONFIRM_MAPPING,
    DISPATCH_SOLAR_STALE, DISPATCH_SOLAR_STALE_STOP, DISPATCH_SOLAR_STABILIZING,
    DISPATCH_SOLAR_LOAD_UNKNOWN, DISPATCH_SOLAR_WAIT, DISPATCH_SOLAR_START, DISPATCH_SOLAR_STOP,
    DISPATCH_SOLAR_RESTART, DISPATCH_SOLAR_SHED_DELAY,
    DISPATCH_SOLAR_COVERED, DISPATCH_PAID_COVERAGE, DISPATCH_PAID_START,
    DISPATCH_SOLAR_OR_PAID_REQUIRED, DISPATCH_SOLAR_DEFICIT_PENDING,
    DISPATCH_SOLAR_SETTLING,
    DISPATCH_DAYLIGHT_WAIT, DISPATCH_DAYLIGHT_DARK, DISPATCH_DAYLIGHT_BRIDGE,
    DISPATCH_DAYLIGHT_EXPIRED, DISPATCH_DAYLIGHT_BUDGET, DISPATCH_DAYLIGHT_EVIDENCE,
    // Values 42 and 43 are reserved by the controller's existing activity records.
    DISPATCH_POWER_TEST = DISPATCH_DAYLIGHT_EVIDENCE + 3,
    DISPATCH_PAID_DISABLED, DISPATCH_PAID_RESERVE, DISPATCH_SOURCE_GRACE,
    DISPATCH_START_WINDOW_SHORT,
} dispatch_reason;

typedef struct {
    int32_t grid_watts;
    uint16_t start_reserve;
    uint32_t next_change;
} dispatch_surplus;

typedef struct {
    uint32_t seconds, seconds_to_sunset, forecast_rate;
    bool enabled, daylight, recent_solar;
} dispatch_daylight_bridge;

typedef struct {
    uint32_t production, supported_production, running_watts, committed_watts;
    uint16_t reserve, shed_margin, start_watts[DISPATCH_LIMIT];
    uint16_t restart_seconds, shed_wait_seconds;
    uint8_t reserved_start_mask;
    bool configured, fresh, sustained, load_known, low, shed_needs_reading;
    dispatch_daylight_bridge bridge;
} dispatch_generation;

typedef enum { DISPATCH_KEEP, DISPATCH_OFF, DISPATCH_ON } dispatch_action;

typedef struct {
    bool present;
    bool fresh;
    bool on;
    uint32_t since;
} dispatch_outlet;

typedef struct {
    uint16_t start_minute;
    uint16_t end_minute;
    uint32_t rate_millicents_per_kwh;
    uint32_t daily_allowance_millicents;
} dispatch_policy;

typedef struct {
    uint16_t paid_start, paid_end;
    uint16_t weekend_paid_start, weekend_paid_end;
    bool current_format, paid_enabled;
} dispatch_operating_plan;

typedef struct {
    uint16_t start_minute, end_minute;
    bool enabled;
} dispatch_quiet_hours;

typedef struct {
    uint16_t start_minute, end_minute;
    uint32_t rate_millicents_per_kwh;
    bool enabled;
} dispatch_rate_window;

typedef struct {
    dispatch_rate_window window[DISPATCH_RATE_WINDOWS];
} dispatch_tariff;

typedef struct {
    uint16_t start_minute, end_minute;
    bool enabled;
    dispatch_quiet_hours quiet;
    dispatch_tariff tariff;
} dispatch_weekend_plan;

typedef bool (*dispatch_local_time)(uint64_t epoch, uint32_t *day, uint32_t *minute);

typedef struct {
    dispatch_action action;
    dispatch_reason reason;
} dispatch_decision;

typedef enum {
    DISPATCH_PRESET_KEEP, DISPATCH_PRESET_LOWER, DISPATCH_PRESET_RESTORE, DISPATCH_PRESET_BRIDGE_LOWER,
} dispatch_solar_preset_action;
enum { DISPATCH_BRIDGE_PRESET_MIN_SECONDS = 120 };

typedef struct {
    uint32_t draw, default_watts, reduced_watts;
    bool ready, reduced, bridge_efficient;
} dispatch_solar_preset_slot;

typedef struct {
    dispatch_solar_preset_action action;
    unsigned slot;
    uint32_t reserve_watts;
} dispatch_solar_preset_decision;

typedef struct {
    dispatch_outlet outlet[DISPATCH_LIMIT];
    uint32_t solar_off_since[DISPATCH_LIMIT];
    uint16_t draw_watts[DISPATCH_LIMIT];
    dispatch_operating_plan operating;
    dispatch_policy policy;
    dispatch_quiet_hours quiet;
    uint8_t priority[DISPATCH_LIMIT];
    uint8_t manual_hold_mask, recovery_hold_mask, productive_mask;
    uint32_t now;
    uint32_t next_start;
    uint32_t minute;
    uint64_t used_nano_cents;
    bool clock_valid;
    bool paused;
    bool accounting_valid;
    bool grid_source, source_grace, grid_recent_export;
    uint32_t guard_rate;
    dispatch_surplus surplus;
    dispatch_generation solar;
} dispatch_inputs;

// Meter readings and budget progress form one persistent snapshot.
typedef struct {
    uint32_t paid_watts, runway_seconds, seconds_to_sunset;
    bool known;
} dispatch_gap_forecast;

typedef struct {
    uint64_t total_mwh;
    uint64_t epoch;
} dispatch_meter_anchor;

typedef struct {
    dispatch_meter_anchor anchor[DISPATCH_LIMIT];
    uint64_t used_nano_cents;
    uint32_t day;
    uint8_t gap_mask;
} dispatch_budget;

typedef struct {
    uint64_t metered_mwh, solar_mwh;
} dispatch_energy_sample;

void dispatch_plan(const dispatch_inputs *input, dispatch_decision output[DISPATCH_LIMIT]);
dispatch_reason dispatch_paid_permission(const dispatch_inputs *in);
dispatch_reason dispatch_bridge_permission(const dispatch_inputs *in);
dispatch_reason dispatch_operation_limit(const dispatch_inputs *in);
dispatch_gap_forecast dispatch_daylight_forecast(const dispatch_inputs *in, uint32_t savings_watts);
bool dispatch_paid_room(const dispatch_inputs *in, uint32_t extra_watts);
bool dispatch_paid_covering(dispatch_reason permission);
int64_t dispatch_solar_headroom(const dispatch_generation *solar);
bool dispatch_solar_deficit(const dispatch_generation *solar);
uint32_t dispatch_solar_coverage(uint32_t production, uint32_t reserve, uint32_t draw);
uint32_t dispatch_solar_restart_remaining(const dispatch_inputs *in, unsigned i);
dispatch_solar_preset_decision dispatch_solar_choose_preset(const dispatch_inputs *in,
    const dispatch_decision decisions[DISPATCH_LIMIT], const dispatch_solar_preset_slot slots[DISPATCH_LIMIT],
    bool confirmed_shortfall);
bool dispatch_quiet_active(const dispatch_inputs *input);
const char *dispatch_explanation(dispatch_reason reason);
bool dispatch_tariff_valid(const dispatch_tariff *tariff);
uint32_t dispatch_highest_rate(uint32_t base, const dispatch_tariff *tariff);
uint32_t dispatch_rate_at(uint32_t base, const dispatch_tariff *tariff, uint32_t minute, unsigned *active_window);
bool dispatch_is_weekend(uint32_t local_day);
void dispatch_apply_weekend(dispatch_inputs *input, const dispatch_weekend_plan *weekend, uint32_t local_day);
uint32_t dispatch_meter_rate(uint32_t base, const dispatch_tariff *tariff,
                             const dispatch_tariff *weekend, uint64_t from, uint64_t to,
                             dispatch_local_time local, bool *used_highest);
bool dispatch_budget_day(dispatch_budget *budget, uint32_t local_day);
void dispatch_budget_carry(dispatch_budget *budget, uint32_t local_day, uint64_t recorded_nano_cents);
uint64_t dispatch_budget_sample(dispatch_budget *budget, unsigned outlet,
                            uint64_t total_mwh, uint64_t epoch, uint32_t rate_millicents);
dispatch_energy_sample dispatch_budget_split_sample(dispatch_budget *budget, unsigned outlet,
    uint64_t total_mwh, uint64_t epoch, uint32_t rate, uint64_t available_solar_mwh);
#endif
