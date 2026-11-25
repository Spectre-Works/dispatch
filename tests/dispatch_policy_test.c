// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "dispatch_policy.h"
#include "json_record.h"

#include <assert.h>
#include <stdio.h>

static void schedules_and_pauses(void)
{
    dispatch_inputs in = {
        .operating = {.paid_enabled = true},
        .solar = {.load_known = true},
        .policy = {22 * 60, 7 * 60, 12000, 10000},
        .now = 180, .minute = 23 * 60, .clock_valid = true, .accounting_valid = true,
    };
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        in.outlet[i] = (dispatch_outlet){true, true, false, 10};
        in.solar.start_watts[i] = 100;
    }
    dispatch_decision out[DISPATCH_LIMIT];
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_ON && out[0].reason == DISPATCH_PAID_START);
    for (unsigned i = 1; i < DISPATCH_LIMIT; ++i) {
        assert(out[i].action == DISPATCH_KEEP && out[i].reason == DISPATCH_START_PENDING);
    }
    in.priority[7] = 2;
    dispatch_plan(&in, out);
    assert(out[7].action == DISPATCH_ON && out[0].reason == DISPATCH_START_PENDING);
    in.manual_hold_mask = 1u << 7;
    dispatch_plan(&in, out);
    assert(out[7].reason == DISPATCH_MANUAL_HOLD && out[0].action == DISPATCH_ON);
    in.priority[7] = 0;
    in.manual_hold_mask = 0;
    in.minute = 0;
    in.outlet[0].on = true;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[1].action == DISPATCH_ON);
    in.minute = 7 * 60;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_WINDOW_CLOSED);
    in.outlet[0].since = in.now;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_WINDOW_CLOSED);
    in.clock_valid = false;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[0].reason == DISPATCH_CLOCK);
    in.paused = true;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_PAUSED);
    in.outlet[0].fresh = false;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[0].reason == DISPATCH_UNOBSERVED);
}

static void allowance_and_counter(void)
{
    dispatch_budget b = {0};
    assert(!dispatch_budget_day(&b, 0));
    assert(dispatch_budget_day(&b, 21000));
    assert(!dispatch_budget_sample(&b, 0, 200000, 10000, 12000));
    dispatch_budget_sample(&b, 1, 200000, 10000, 12000);
    assert(!b.used_nano_cents && !b.gap_mask);
    assert(dispatch_budget_sample(&b, 0, 211000, 10060, 12000) == 11000);
    dispatch_budget_sample(&b, 1, 210000, 10060, 12000);
    assert(b.used_nano_cents == 252000000);
    dispatch_budget persisted = b;
    assert(dispatch_budget_day(&persisted, b.day));
    dispatch_budget_sample(&persisted, 0, 215000, 10090, 12000);
    assert(persisted.used_nano_cents == 300000000);
    assert(!dispatch_budget_day(&persisted, b.day - 1));
    assert(!dispatch_budget_sample(&persisted, 0, 0, 10120, 12000));
    assert(persisted.used_nano_cents == 300000000 && persisted.gap_mask == 1);
    dispatch_budget_sample(&persisted, 1, 600000, 10121, 12000);
    assert(persisted.gap_mask == 3);
    assert(dispatch_budget_day(&persisted, 21001));
    assert(!persisted.used_nano_cents && !persisted.gap_mask && persisted.anchor[1].epoch == 10121);
    dispatch_budget_sample(&persisted, 1, 605000, 10140, 12000);
    assert(persisted.used_nano_cents == 60000000);

    dispatch_inputs in = {
        .operating = {.paid_enabled = true},
        .solar = {.load_known = true, .start_watts = {100, 100, 100, 100}},
        .outlet = {{true, true, true, 0}, {true, true, false, 0}},
        .policy = {0, 0, 12000, 300},
        .now = 140, .used_nano_cents = 300000000, .clock_valid = true, .accounting_valid = true,
    };
    dispatch_decision out[DISPATCH_LIMIT];
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_BUDGET_USED);
    assert(out[1].action == DISPATCH_KEEP && out[1].reason == DISPATCH_BUDGET_USED);
    in.used_nano_cents--;
    in.accounting_valid = false;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_BUDGET_UNKNOWN);
}

static void budget_date_correction(void)
{
    dispatch_budget budget = {
        .day = 21000, .used_nano_cents = 300000000, .gap_mask = 1,
        .anchor = {{200000, 10000}, {200000, 10000}},
    };
    dispatch_budget_carry(&budget, 20999, 100000000);
    assert(budget.day == 20999 && budget.used_nano_cents == 300000000 && budget.gap_mask == 1);
    assert(budget.anchor[0].total_mwh == 200000 && budget.anchor[0].epoch == 10000);
    assert(dispatch_budget_sample(&budget, 1, 205000, 10060, 12000) == 5000);
    assert(budget.used_nano_cents == 360000000);
    dispatch_budget_carry(&budget, 21000, 400000000);
    assert(budget.day == 21000 && budget.used_nano_cents == 400000000 && budget.gap_mask == 1);
    dispatch_budget_carry(&budget, 20999, 100000000);
    assert(budget.used_nano_cents == 400000000);
    assert(dispatch_budget_day(&budget, 21000));
    assert(!budget.used_nano_cents && !budget.gap_mask && budget.anchor[1].epoch == 10060);
}

static void quiet_hours(void)
{
    dispatch_inputs in = {
        .operating = {.paid_enabled = true},
        .solar = {.load_known = true, .start_watts = {100, 100, 100, 100}},
        .outlet = {{true, true, true, 0}, {true, true, false, 0}},
        .policy = {0, 0, 12000, 300},
        .quiet = {22 * 60, 7 * 60, true},
        .now = 180, .minute = 22 * 60, .clock_valid = true, .accounting_valid = true,
    };
    dispatch_decision out[DISPATCH_LIMIT];

    dispatch_plan(&in, out);
    assert(dispatch_quiet_active(&in));
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_QUIET_HOURS);
    assert(out[1].action == DISPATCH_KEEP && out[1].reason == DISPATCH_QUIET_HOURS);

    in.minute = 0;
    in.outlet[0].since = in.now - 30;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_QUIET_HOURS);
    in.paused = true;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_PAUSED);
    in.paused = false;

    in.minute = 7 * 60;
    in.used_nano_cents = 300000000;
    dispatch_plan(&in, out);
    assert(!dispatch_quiet_active(&in));
    assert(out[1].action == DISPATCH_KEEP && out[1].reason == DISPATCH_BUDGET_USED);
    in.used_nano_cents = 0;
    in.policy = (dispatch_policy){8 * 60, 21 * 60, 12000, 300};
    dispatch_plan(&in, out);
    assert(out[1].action == DISPATCH_KEEP && out[1].reason == DISPATCH_WINDOW_CLOSED);
    in.minute = 8 * 60;
    dispatch_plan(&in, out);
    assert(out[1].action == DISPATCH_ON);

    in.quiet = (dispatch_quiet_hours){9 * 60, 10 * 60, true};
    in.minute = 9 * 60;
    assert(dispatch_quiet_active(&in));
    in.clock_valid = false;
    dispatch_plan(&in, out);
    assert(!dispatch_quiet_active(&in));
    assert(out[1].action == DISPATCH_KEEP && out[1].reason == DISPATCH_CLOCK);
    in.clock_valid = true;
    in.quiet.enabled = false;
    dispatch_plan(&in, out);
    assert(!dispatch_quiet_active(&in) && out[1].action == DISPATCH_ON);
}

static bool test_local_time(uint64_t epoch, uint32_t *day, uint32_t *minute)
{
    *day = epoch / 86400;
    *minute = epoch / 60 % 1440;
    return true;
}

static bool spring_forward_time(uint64_t epoch, uint32_t *day, uint32_t *minute)
{
    // Clock jumps from 01:59 to 03:00. The 02:00 rate does not apply.
    return test_local_time(epoch + (epoch >= 7200 ? 3600 : 0), day, minute);
}

static void rate_windows(void)
{
    const uint32_t base = 12000;
    dispatch_tariff rates = {.window = {{22 * 60, 7 * 60, 5000, true}, {16 * 60, 22 * 60, 22000, true}}};
    assert(dispatch_tariff_valid(&rates));
    unsigned active;
    assert(dispatch_rate_at(base, &rates, 0, &active) == 5000 && active == 1);
    assert(dispatch_rate_at(base, &rates, 7 * 60, &active) == base && active == 0);
    assert(dispatch_rate_at(base, &rates, 16 * 60, &active) == 22000 && active == 2);
    assert(dispatch_rate_at(base, &rates, 22 * 60, &active) == 5000 && active == 1);
    rates.window[1].end_minute++;
    assert(!dispatch_tariff_valid(&rates));
    rates.window[1].end_minute = rates.window[1].start_minute;
    assert(!dispatch_tariff_valid(&rates));
    rates.window[1].enabled = false;
    assert(dispatch_tariff_valid(&rates));
    rates.window[1] = (dispatch_rate_window){16 * 60, 22 * 60, 22000, true};

    const uint64_t boundary = 22 * 3600;
    bool highest;
    assert(dispatch_meter_rate(base, &rates, NULL, boundary - 10, boundary, test_local_time, &highest) == 22000 && !highest);
    assert(dispatch_meter_rate(base, &rates, NULL, boundary, boundary + 10, test_local_time, &highest) == 5000 && !highest);
    assert(dispatch_meter_rate(base, &rates, NULL, boundary - 9, boundary + 4, test_local_time, &highest) == 16770 && !highest);
    assert(dispatch_meter_rate(base, &rates, NULL, boundary - 150, boundary + 150, test_local_time, &highest) == 13500 && !highest);
    assert(dispatch_meter_rate(base, &rates, NULL, boundary - 151, boundary + 150, test_local_time, &highest) == 22000 && highest);
    assert(dispatch_meter_rate(base, &rates, NULL, 86399, 86401, test_local_time, &highest) == 5000 && !highest);
    assert(dispatch_meter_rate(base, &rates, NULL, boundary, boundary, test_local_time, &highest) == 5000 && !highest);

    dispatch_budget budget = {0};
    assert(dispatch_budget_day(&budget, 1));
    dispatch_budget_sample(&budget, 0, 100000, boundary - 5, base);
    const uint32_t cost_rate = dispatch_meter_rate(base, &rates, NULL, boundary - 5, boundary + 5, test_local_time, &highest);
    dispatch_budget_sample(&budget, 0, 104000, boundary + 5, cost_rate);
    assert(budget.used_nano_cents == 54000000 && !budget.gap_mask);

    rates = (dispatch_tariff){.window = {{120, 180, 100, true}, {180, 240, 6000, true}}};
    assert(dispatch_meter_rate(base, &rates, NULL, 7190, 7210, spring_forward_time, &highest) == 9000 && !highest);
}

static void weekend_rates(void)
{
    const dispatch_tariff weekday = {.window = {{1320, 420, 18000, true}}};
    const dispatch_tariff weekend = {.window = {{1200, 540, 8000, true}}};
    const uint64_t saturday = 2 * 86400;
    const uint64_t monday = 4 * 86400;
    bool highest;
    assert(!dispatch_is_weekend(1) && dispatch_is_weekend(2));
    assert(dispatch_is_weekend(3) && !dispatch_is_weekend(4));
    assert(dispatch_meter_rate(12000, &weekday, &weekend, saturday - 5, saturday + 5,
                               test_local_time, &highest) == 13000 && !highest);
    assert(dispatch_meter_rate(12000, &weekday, &weekend, monday - 1, monday + 3,
                               test_local_time, &highest) == 15500 && !highest);
    assert(dispatch_meter_rate(12000, &weekday, &weekend, saturday, saturday,
                               test_local_time, &highest) == 8000 && !highest);
    assert(dispatch_meter_rate(12000, &weekend, &weekday, monday - 301, monday,
                               test_local_time, &highest) == 18000 && highest);
}

static void weekend_schedule(void)
{
    const dispatch_inputs daily = {
        .operating = {.paid_enabled = true},
        .solar = {.load_known = true, .start_watts = {100, 100, 100, 100}},
        .outlet = {{true, true, true, 0}},
        .policy = {1320, 420, 12000, 400},
        .now = 200, .minute = 0, .clock_valid = true, .accounting_valid = true,
    };
    dispatch_weekend_plan weekend = {
        .enabled = true, .start_minute = 480, .end_minute = 720, .quiet = {480, 540, true},
    };
    dispatch_inputs in = daily;
    dispatch_decision out[DISPATCH_LIMIT];
    dispatch_apply_weekend(&in, &weekend, 2);
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_WINDOW_CLOSED);
    assert(in.policy.daily_allowance_millicents == 400 && in.policy.rate_millicents_per_kwh == 12000);
    in.minute = 480;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_QUIET_HOURS);
    in = daily;
    dispatch_apply_weekend(&in, &weekend, 4);
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[0].reason == DISPATCH_PAID_COVERAGE);
    weekend.enabled = false;
    dispatch_apply_weekend(&in, &weekend, 2);
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[0].reason == DISPATCH_PAID_COVERAGE);
}

static void json_boundaries(void)
{
    json_record doc;
    double n;
    assert(json_parse(&doc, "{ \"nested\": [ {\"output\": false, \"total\":123.125} ], \"null\": null }"));
    const unsigned first = json_first(&doc, json_field(&doc, 1, "nested"));
    assert(json_equals(&doc, json_field(&doc, first, "output"), "false"));
    assert(json_number(&doc, json_field(&doc, first, "total"), &n) && n == 123.125);
    assert(!json_number(&doc, json_field(&doc, 1, "null"), &n));
    assert(json_parse(&doc, "{\"output\":true,\"output\":false}"));
    assert(!json_field(&doc, 1, "output"));
    assert(!json_parse(&doc, "{\"bad\":\"\\u0"));
    assert(!json_parse(&doc, "{\"bad\":01}"));
    assert(!json_parse(&doc, "{\"bad\":1e+}"));
    assert(!json_parse(&doc, "[[[[[[[[[[[5]]]]]]]]]]]"));
    assert(!json_parse(&doc, "true false"));
}

static void coordination(void)
{
    dispatch_inputs in = {
        .operating = {.paid_enabled = true},
        .solar = {.load_known = true, .start_watts = {100, 100, 100, 100}},
        .outlet = {{true, true, false, 0}, {true, true, false, 0}, {true, true, false, 0}},
        .clock_valid = true, .accounting_valid = true, .now = 180,
        .priority = {0, 2, 1, 0},
    };
    dispatch_decision out[DISPATCH_LIMIT];
    dispatch_plan(&in, out);
    assert(out[1].action == DISPATCH_ON && out[0].reason == DISPATCH_START_PENDING);
    in.manual_hold_mask = 1u << 1;
    dispatch_plan(&in, out);
    assert(out[1].reason == DISPATCH_MANUAL_HOLD && out[2].action == DISPATCH_ON);
    in.recovery_hold_mask = 1u << 2;
    dispatch_plan(&in, out);
    assert(out[2].reason == DISPATCH_RECOVERY_HOLD && out[0].action == DISPATCH_ON);
    in.priority[2] = UINT8_MAX;
    in.recovery_hold_mask = 0;
    in.next_start = 200;
    dispatch_plan(&in, out);
    assert(out[2].action == DISPATCH_KEEP && out[2].reason == DISPATCH_START_PENDING);
    in.now = 210;
    dispatch_plan(&in, out);
    assert(out[2].action == DISPATCH_ON);
    in.outlet[1].on = true;
    in.outlet[1].since = 200;
    in.paused = true;
    dispatch_plan(&in, out);
    assert(out[1].action == DISPATCH_OFF && out[1].reason == DISPATCH_PAUSED);
    assert(out[2].action == DISPATCH_KEEP && out[2].reason == DISPATCH_PAUSED);
    in.paused = false;
    in.policy.daily_allowance_millicents = 1;
    in.used_nano_cents = 1000000;
    dispatch_plan(&in, out);
    assert(out[2].action == DISPATCH_KEEP && out[2].reason == DISPATCH_BUDGET_USED);
}

static void solar_budget_split(void)
{
    dispatch_budget budget = {0};
    assert(dispatch_budget_day(&budget, 8));
    dispatch_energy_sample result = dispatch_budget_split_sample(&budget, 0, 10000, 60, 12000, 99999);
    assert(!result.metered_mwh && !result.solar_mwh && !budget.used_nano_cents);
    result = dispatch_budget_split_sample(&budget, 0, 18000, 96, 12000, 5000);
    assert(result.metered_mwh == 8000 && result.solar_mwh == 5000 && budget.used_nano_cents == 36000000);
    result = dispatch_budget_split_sample(&budget, 0, 20000, 106, 5000, 3000);
    assert(result.metered_mwh == 2000 && result.solar_mwh == 2000 && budget.used_nano_cents == 36000000);
    result = dispatch_budget_split_sample(&budget, 0, 24000, 407, 5000, 4000);
    assert(result.metered_mwh == 4000 && !result.solar_mwh && budget.used_nano_cents == 56000000);
    result = dispatch_budget_split_sample(&budget, 0, 1000, 408, 5000, 1000);
    assert(!result.metered_mwh && !result.solar_mwh && budget.gap_mask == 1);
    assert(dispatch_budget_day(&budget, 9) && !budget.used_nano_cents && !budget.gap_mask);
}

static void solar_presets(void)
{
    dispatch_inputs in = {
        .outlet = {{true, true, true, 5}, {true, true, true, 5}},
        .priority = {2, 1}, .clock_valid = true, .now = 800,
        .solar = {.configured = true, .fresh = true, .load_known = true, .sustained = true,
                  .reserve = 200, .running_watts = 1460, .supported_production = 1550},
    };
    dispatch_decision out[DISPATCH_LIMIT] = {{DISPATCH_KEEP, DISPATCH_PAID_COVERAGE}, {DISPATCH_OFF, DISPATCH_SOLAR_STOP}};
    dispatch_solar_preset_slot slots[DISPATCH_LIMIT] = {
        {.ready = true, .draw = 730, .default_watts = 745, .reduced_watts = 555},
        {.ready = true, .draw = 730, .default_watts = 745, .reduced_watts = 555},
    };
    assert(dispatch_solar_choose_preset(&in, out, slots, false).action == DISPATCH_PRESET_KEEP);
    dispatch_solar_preset_decision decision = dispatch_solar_choose_preset(&in, out, slots, true);
    assert(decision.action == DISPATCH_PRESET_LOWER && decision.slot == 1);
    in.recovery_hold_mask = 1u << 1;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).slot == 0);
    in.recovery_hold_mask = 0;
    in.solar.supported_production = 1200;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).action == DISPATCH_PRESET_KEEP);
    in.solar.supported_production = 1550;
    out[1].reason = DISPATCH_BUDGET_USED;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).action == DISPATCH_PRESET_KEEP);

    out[1].action = DISPATCH_KEEP;
    slots[0].reduced = slots[1].reduced = true;
    slots[0].draw = slots[1].draw = 540;
    in.solar.running_watts = 1080;
    in.solar.supported_production = 1550;
    decision = dispatch_solar_choose_preset(&in, out, slots, false);
    assert(decision.action == DISPATCH_PRESET_RESTORE && decision.slot == 0 && decision.reserve_watts == 205);
    in.solar.committed_watts = 16;
    assert(dispatch_solar_choose_preset(&in, out, slots, false).action == DISPATCH_PRESET_KEEP);
    in.solar.committed_watts = 0;
    out[2] = (dispatch_decision){DISPATCH_ON, DISPATCH_SOLAR_START};
    assert(dispatch_solar_choose_preset(&in, out, slots, false).action == DISPATCH_PRESET_KEEP);
    out[2].action = DISPATCH_KEEP;
    in.solar.fresh = false;
    assert(dispatch_solar_choose_preset(&in, out, slots, false).action == DISPATCH_PRESET_KEEP);
    in.solar.fresh = true;
    in.paused = true;
    assert(dispatch_solar_choose_preset(&in, out, slots, false).action == DISPATCH_PRESET_KEEP);
}

static void daylight_gap_coverage(void)
{
    dispatch_inputs in = {
        .outlet = {{true, true, true, 5}, {true, true, false, 10}},
        .clock_valid = true, .accounting_valid = true, .now = 800, .minute = 970,
        .policy = {0, 0, 13900, 22000}, .guard_rate = 13900,
        .solar = {.configured = true, .fresh = true, .load_known = true, .sustained = true,
            .start_watts = {600, 600}, .reserve = 500, .production = 722, .supported_production = 720, .running_watts = 540,
            .bridge = {.enabled = true, .daylight = true, .forecast_rate = 13900, .seconds = 120}},
    };
    dispatch_decision out[DISPATCH_LIMIT];
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_BRIDGE);
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[0].reason == DISPATCH_DAYLIGHT_BRIDGE);
    assert(out[1].action == DISPATCH_KEEP && out[1].reason == DISPATCH_DAYLIGHT_WAIT);
    in.grid_source = true;
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_BRIDGE);
    in.grid_source = false;
    in.solar.fresh = false;
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_BRIDGE);
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[0].reason == DISPATCH_DAYLIGHT_BRIDGE);
    assert(out[1].action == DISPATCH_KEEP);
    in.solar.load_known = false;
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_EVIDENCE);
    in.source_grace = true;
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_EVIDENCE);
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[1].action == DISPATCH_KEEP);
    in.solar.fresh = true;
    in.solar.load_known = true;
    in.used_nano_cents = 21990000000ULL;
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_BUDGET);
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_DAYLIGHT_BUDGET);
    in.used_nano_cents = 22000000000ULL;
    assert(dispatch_paid_permission(&in) == DISPATCH_BUDGET_USED);
    in.used_nano_cents = 0;
    in.accounting_valid = false;
    assert(dispatch_paid_permission(&in) == DISPATCH_BUDGET_UNKNOWN);
    in.accounting_valid = true;
    in.solar.bridge.seconds = 0;
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_EXPIRED);
    in.solar.bridge.daylight = false;
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_DARK);
    in.solar.bridge.daylight = true;
    in.solar.bridge.seconds = 120;
    in.quiet = (dispatch_quiet_hours){960, 1000, true};
    assert(dispatch_paid_permission(&in) == DISPATCH_QUIET_HOURS);
    in.quiet.enabled = false;
    in.paused = true;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_PAUSED);

    in.paused = false;
    in.solar.production = in.solar.supported_production = 1700;
    in.solar.start_watts[1] = 600;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[0].reason == DISPATCH_SOLAR_COVERED);
    assert(out[1].action == DISPATCH_ON && out[1].reason == DISPATCH_SOLAR_START);
    in.solar.bridge.enabled = false;
    assert(dispatch_paid_permission(&in) == DISPATCH_PAID_DISABLED);
}

static void daylight_bridge_presets(void)
{
    dispatch_inputs in = {
        .outlet = {{true, true, true, 5}, {true, true, true, 5}},
        .priority = {2, 1}, .clock_valid = true, .accounting_valid = true, .now = 800,
        .solar = {.configured = true, .fresh = true, .load_known = true, .sustained = true,
                  .reserve = 200, .running_watts = 1460, .production = 1100, .supported_production = 1100,
                  .bridge = {.enabled = true, .daylight = true, .seconds = 300}},
    };
    dispatch_decision out[DISPATCH_LIMIT];
    dispatch_solar_preset_slot slots[DISPATCH_LIMIT] = {
        {.ready = true, .draw = 730, .default_watts = 745, .reduced_watts = 510, .bridge_efficient = true},
        {.ready = true, .draw = 730, .default_watts = 745, .reduced_watts = 555, .bridge_efficient = true},
    };
    dispatch_plan(&in, out);
    assert(out[1].reason == DISPATCH_DAYLIGHT_BRIDGE);
    assert(dispatch_solar_choose_preset(&in, out, slots, false).action == DISPATCH_PRESET_KEEP);
    dispatch_solar_preset_decision pick = dispatch_solar_choose_preset(&in, out, slots, true);
    assert(pick.action == DISPATCH_PRESET_BRIDGE_LOWER && pick.slot == 1);
    in.priority[1] = 2;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).slot == 0);
    slots[0].bridge_efficient = false;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).slot == 1);
    in.manual_hold_mask = 1u << 1;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).action == DISPATCH_PRESET_KEEP);
    in.manual_hold_mask = 0;
    in.solar.bridge.seconds = DISPATCH_BRIDGE_PRESET_MIN_SECONDS - 1;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).action == DISPATCH_PRESET_KEEP);
    in.solar.bridge.seconds = 300;
    in.solar.fresh = false;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).action == DISPATCH_PRESET_KEEP);
    in.solar.fresh = true;
    out[0] = (dispatch_decision){DISPATCH_OFF, DISPATCH_QUIET_HOURS};
    assert(dispatch_solar_choose_preset(&in, out, slots, true).action == DISPATCH_PRESET_KEEP);
}

static void unified_operating_plan(void)
{
    dispatch_inputs in = {
        .outlet = {{true, true, true, 0}, {true, true, false, 0}},
        .draw_watts = {730},
        .policy = {0, 0, 12000, 22000},
        .operating = {.paid_enabled = true, .paid_start = 0, .paid_end = 360},
        .now = 1000, .minute = 800, .clock_valid = true, .accounting_valid = true,
        .guard_rate = 12000,
        .solar = {.configured = true, .fresh = true, .sustained = true, .load_known = true,
                  .production = 400, .supported_production = 400, .running_watts = 730, .start_watts = {780, 320},
                  .bridge = {.enabled = true, .daylight = true, .seconds = 600}},
    };
    dispatch_decision out[DISPATCH_LIMIT];
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[0].reason == DISPATCH_DAYLIGHT_BRIDGE);
    assert(out[1].action == DISPATCH_KEEP && out[1].reason == DISPATCH_DAYLIGHT_WAIT);
    const dispatch_solar_preset_slot presets[DISPATCH_LIMIT] = {
        {.ready = true, .draw = 730, .default_watts = 750, .reduced_watts = 540, .bridge_efficient = true},
    };
    assert(dispatch_solar_choose_preset(&in, out, presets, true).action == DISPATCH_PRESET_BRIDGE_LOWER);

    // Overlapping permissions allow starts, use the same budget and prefer the paid-window explanation.
    in.minute = 300;
    dispatch_plan(&in, out);
    assert(out[0].reason == DISPATCH_PAID_COVERAGE && out[1].reason == DISPATCH_PAID_START);
    assert(dispatch_solar_choose_preset(&in, out, presets, true).action == DISPATCH_PRESET_LOWER);
    in.minute = 359;
    dispatch_plan(&in, out);
    assert(out[1].reason == DISPATCH_START_WINDOW_SHORT);
    in.minute = 300;
    in.used_nano_cents = 22000000000ULL - 1;
    dispatch_plan(&in, out);
    assert(out[0].reason == DISPATCH_PAID_RESERVE && out[0].action == DISPATCH_OFF);
    in.solar.production = in.solar.supported_production = 1500;
    dispatch_plan(&in, out);
    assert(out[0].reason == DISPATCH_SOLAR_COVERED && out[1].reason == DISPATCH_SOLAR_START);

    // Quiet Hours and the hard mining schedule apply even while paid power is available.
    in.quiet = (dispatch_quiet_hours){250, 350, true};
    in.outlet[0].since = in.now - 30;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_QUIET_HOURS);
    in.quiet.enabled = false;
    in.policy.start_minute = 500;
    in.policy.end_minute = 1200;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[1].reason == DISPATCH_WINDOW_CLOSED);

    in.policy.start_minute = in.policy.end_minute = 0;
    in.outlet[0].since = 0;
    in.minute = 800;
    in.used_nano_cents = 0;
    in.solar.fresh = false;
    in.solar.bridge.enabled = false;
    in.source_grace = true;
    dispatch_plan(&in, out);
    assert(out[0].reason == DISPATCH_SOURCE_GRACE && out[1].action == DISPATCH_KEEP);
    in.source_grace = false;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[1].action == DISPATCH_KEEP);
    in.minute = 300;
    dispatch_plan(&in, out);
    assert(out[0].reason == DISPATCH_PAID_COVERAGE && out[1].reason == DISPATCH_PAID_START);

    in.grid_source = true;
    in.solar.bridge.enabled = true;
    in.solar.fresh = true;
    in.solar.production = in.solar.supported_production = 1500;
    in.surplus.grid_watts = -770;
    in.surplus.start_reserve = 150;
    in.minute = 800;
    dispatch_plan(&in, out);
    assert(out[1].reason == DISPATCH_SURPLUS_START);
    in.surplus.start_reserve = 480;
    dispatch_plan(&in, out);
    assert(out[1].reason == DISPATCH_SURPLUS_WAIT);

    in.solar.bridge.seconds = 0;
    in.outlet[1].on = true;
    in.solar.running_watts = 1050;
    in.solar.production = in.solar.supported_production = 600;
    in.draw_watts[1] = 320;
    in.productive_mask = 1;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_KEEP && out[1].action == DISPATCH_OFF);
    in.priority[1] = 2;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[1].action == DISPATCH_KEEP);

    in.operating.weekend_paid_start = 60;
    in.operating.weekend_paid_end = 120;
    const dispatch_weekend_plan weekend = {.enabled = true};
    dispatch_apply_weekend(&in, &weekend, 2); // Saturday.
    assert(in.operating.paid_start == 60 && in.operating.paid_end == 120);
}

static void daylight_budget_until_sunset(void)
{
    dispatch_inputs in = {
        .outlet = {{true, true, true, 0}},
        .now = 1000, .clock_valid = true, .accounting_valid = true,
        .used_nano_cents = 18000000000ULL, .guard_rate = 13900,
        .policy = {.daily_allowance_millicents = 22000},
        .solar = {.configured = true, .fresh = true, .load_known = true, .sustained = true,
                  .running_watts = 730, .production = 400, .supported_production = 400,
                  .bridge = {.enabled = true, .daylight = true, .forecast_rate = 13900,
                             .seconds = 3600, .seconds_to_sunset = 5400}},
    };
    dispatch_decision out[DISPATCH_LIMIT];
    dispatch_solar_preset_slot slots[DISPATCH_LIMIT] = {
        {.ready = true, .draw = 730, .reduced_watts = 540, .bridge_efficient = true},
    };
    dispatch_gap_forecast before = dispatch_daylight_forecast(&in, 0);
    dispatch_gap_forecast after = dispatch_daylight_forecast(&in, 190);
    assert(before.known && before.paid_watts == 330 && before.runway_seconds == 3139);
    assert(after.known && after.paid_watts == 140 && after.runway_seconds == 7399);
    assert(before.runway_seconds < before.seconds_to_sunset && after.runway_seconds > after.seconds_to_sunset);
    dispatch_plan(&in, out);
    assert(dispatch_solar_choose_preset(&in, out, slots, true).action == DISPATCH_PRESET_BRIDGE_LOWER);

    // The fleet is funded in the next example. Keep Default unless lowering removes paid draw.
    in.used_nano_cents = 16000000000ULL;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).action == DISPATCH_PRESET_BRIDGE_LOWER);
    in.used_nano_cents = 15000000000ULL;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).action == DISPATCH_PRESET_KEEP);
    slots[0].reduced_watts = 400;
    assert(dispatch_solar_choose_preset(&in, out, slots, true).action == DISPATCH_PRESET_BRIDGE_LOWER);

    in.solar.bridge.seconds = 0;
    dispatch_plan(&in, out);
    assert(out[0].action == DISPATCH_OFF && out[0].reason == DISPATCH_DAYLIGHT_EXPIRED);
    in.solar.bridge.seconds = 3600;
    in.solar.bridge.seconds_to_sunset = 0;
    assert(!dispatch_daylight_forecast(&in, 0).known);
    in.solar.bridge.seconds_to_sunset = 5400;
    in.accounting_valid = false;
    assert(!dispatch_daylight_forecast(&in, 0).known);
}

int main(void)
{
    schedules_and_pauses();
    allowance_and_counter();
    budget_date_correction();
    solar_budget_split();
    solar_presets();
    daylight_gap_coverage();
    daylight_bridge_presets();
    daylight_budget_until_sunset();
    unified_operating_plan();
    quiet_hours();
    rate_windows();
    weekend_rates();
    weekend_schedule();
    coordination();
    json_boundaries();
    puts("DISPATCH policy and JSON tests passed");
}
