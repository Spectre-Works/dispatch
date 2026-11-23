// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "dispatch_evidence.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void learn(void)
{
    dispatch_profile p = {0};
    dispatch_profile_sample(&p, 100, false, 999);
    assert(p.hash_samples == 1 && p.mean_mhs == 100 && !p.power_samples);
    dispatch_profile_sample(&p, 200, true, 500);
    dispatch_profile_sample(&p, 300, true, 700);
    assert(p.hash_samples == 3 && p.mean_mhs == 200 && p.min_mhs == 100 && p.max_mhs == 300);
    assert(p.power_samples == 2 && p.mean_watts == 600 && p.min_watts == 500 && p.max_watts == 700);
    dispatch_profile_start(&p, 12);
    dispatch_profile_start(&p, 0);
    assert(p.startups == 2 && p.startup_seconds == 0);
    p.hash_samples = UINT16_MAX;
    dispatch_profile_sample(&p, 232, false, 0);
    assert(p.hash_samples == UINT16_MAX && p.mean_mhs == 201);
}

static void rollup(void)
{
    dispatch_daily_evidence days[DISPATCH_EVIDENCE_DAYS] = {0};
    dispatch_daily_evidence *day = dispatch_evidence_day(days, 20777);
    day->energy_mwh = 120000;
    day->cost_nano_cents = 1200000;
    day->powered_seconds = 3600;
    day->interruptions = 1;
    day->metered = true;
    assert(dispatch_evidence_day(days, 20777)->energy_mwh == 120000);
    assert(dispatch_evidence_day(days, 20778)->day == 20778);
    assert(day->powered_seconds == 3600 && day->interruptions == 1 && day->metered);
    day = dispatch_evidence_day(days, 20777 + DISPATCH_EVIDENCE_DAYS);
    assert(day->day == 20784 && !day->energy_mwh && !day->cost_nano_cents &&
        !day->powered_seconds && !day->interruptions && !day->metered);
}

static void learn_efficiency(void)
{
    dispatch_efficiency p = {0};
    assert(!dispatch_efficiency_sample(&p, 500, 0));
    assert(!dispatch_efficiency_sample(&p, 0, 2500));
    assert(!dispatch_efficiency_sample(&p, NAN, 2500));
    assert(!dispatch_efficiency_sample(&p, 500, INFINITY));
    assert(!p.samples);
    assert(dispatch_efficiency_sample(&p, 500, 2500));
    assert(dispatch_efficiency_sample(&p, 600, 6000));
    assert(p.samples == 2 && p.mean_j_per_gh == 150);
    p.samples = UINT16_MAX;
    assert(dispatch_efficiency_sample(&p, 182, 1000));
    assert(p.samples == UINT16_MAX && p.mean_j_per_gh == 151);
}

static void first_run_power(void)
{
    dispatch_power_test_samples test = {0};
    dispatch_power_test_result result = {0};
    assert(!dispatch_power_test_sample(&test, 5, 843, false, 15, &result));
    assert(!dispatch_power_test_sample(&test, 120, 0, true, 15, &result));
    assert(!dispatch_power_test_sample(&test, 125, 500, true, 15, &result));
    assert(!dispatch_power_test_sample(&test, 130, 730, true, 15, &result));
    assert(!dispatch_power_test_sample(&test, 135, 720, true, 15, &result));
    assert(!dispatch_power_test_sample(&test, 140, 725, true, 15, &result));
    assert(!result.peak);
    assert(dispatch_power_test_sample(&test, 145, 725, true, 15, &result));
    assert(result.peak == 843 && result.running == 725);
}

static void sustained_work(void)
{
    dispatch_work_monitor monitor = {0};
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_MISSING, 10) == DISPATCH_WORK_UNCHANGED);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_MISSING, 129) == DISPATCH_WORK_UNCHANGED);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_MISSING, 130) == DISPATCH_WORK_LOST);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_MISSING, 135) == DISPATCH_WORK_UNCHANGED && monitor.active);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_PRESENT, 140) == DISPATCH_WORK_UNCHANGED);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_MISSING, 150) == DISPATCH_WORK_UNCHANGED);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_PRESENT, 155) == DISPATCH_WORK_UNCHANGED);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_PRESENT, 184) == DISPATCH_WORK_UNCHANGED && monitor.active);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_PRESENT, 185) == DISPATCH_WORK_RESTORED && !monitor.active);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_MISSING, 200) == DISPATCH_WORK_UNCHANGED);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_IDLE, 230) == DISPATCH_WORK_UNCHANGED);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_MISSING, 350) == DISPATCH_WORK_UNCHANGED && !monitor.active);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_MISSING, 470) == DISPATCH_WORK_LOST);
    assert(dispatch_work_update(&monitor, DISPATCH_WORK_IDLE, 475) == DISPATCH_WORK_UNCHANGED && !monitor.active);
}

static void preset_comparison(void)
{
    dispatch_preset_totals a = {0}, b = {0};
    assert(!dispatch_preset_sample(&a, 10, 730, NAN, 730));
    assert(!dispatch_preset_sample(&a, 0, 730, 2200000, 730));
    assert(dispatch_preset_sample(&a, 600, 730, 2200000, 745));
    assert(dispatch_preset_sample(&b, 900, 540, 1800000, 555));
    assert(a.seconds == 600 && a.peak_watts == 745 && a.joules == 438000);
    assert(fabs(dispatch_preset_efficiency(&b) - 300) < 1e-9);
    dispatch_preset_totals combined = a;
    dispatch_preset_merge(&combined, &b);
    assert(combined.seconds == 1500 && combined.peak_watts == 745);
    assert(fabs(dispatch_preset_efficiency(&combined) - 924000.0 / 2940.0) < 1e-9);

    dispatch_preset_evidence normal = {0}, reduced = {0};
    assert(dispatch_preset_compare(&normal, &reduced) == DISPATCH_PRESET_LEARNING);
    normal.totals = a;
    reduced.totals = b;
    for (unsigned i = 0; i < DISPATCH_PRESET_WINDOWS; ++i) {
        dispatch_preset_record_window(&normal, &a);
        dispatch_preset_record_window(&reduced, &b);
    }
    assert(dispatch_preset_compare(&normal, &reduced) == DISPATCH_PRESET_REDUCED);
    dispatch_preset_record_window(&reduced, &a);
    assert(dispatch_preset_compare(&normal, &reduced) == DISPATCH_PRESET_SIMILAR);
    dispatch_preset_record_window(&reduced, &a);
    dispatch_preset_record_window(&reduced, &a);
    for (unsigned i = 0; i < DISPATCH_PRESET_WINDOWS; ++i) dispatch_preset_record_window(&normal, &b);
    assert(dispatch_preset_compare(&normal, &reduced) == DISPATCH_PRESET_SIMILAR);
    normal.totals = b;
    reduced.totals = a;
    assert(dispatch_preset_compare(&normal, &reduced) == DISPATCH_PRESET_DEFAULT);
}

int main(void)
{
    learn();
    first_run_power();
    learn_efficiency();
    preset_comparison();
    sustained_work();
    rollup();
    puts("DISPATCH evidence tests passed");
}
