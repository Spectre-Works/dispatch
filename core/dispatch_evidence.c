// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "dispatch_evidence.h"
#include <math.h>

dispatch_daily_evidence *dispatch_evidence_day(dispatch_daily_evidence days[DISPATCH_EVIDENCE_DAYS], uint32_t day)
{
    dispatch_daily_evidence *entry = &days[day % DISPATCH_EVIDENCE_DAYS];
    if (entry->day != day) {
        *entry = (dispatch_daily_evidence){.day = day};
    }
    return entry;
}

static unsigned sample_weight(uint16_t *samples)
{
    if (*samples < UINT16_MAX) {
        ++*samples;
    }
    return *samples < DISPATCH_PROFILE_WEIGHT ? *samples : DISPATCH_PROFILE_WEIGHT;
}

bool dispatch_power_test_sample(dispatch_power_test_samples *test, uint32_t now,
    uint16_t watts, bool operating, unsigned stable_seconds, dispatch_power_test_result *result)
{
    if (watts > test->peak) test->peak = watts;
    if (!operating || watts < 10) {
        test->count = 0;
        return false;
    }
    const uint16_t high = test->count && test->high > watts ? test->high : watts;
    const uint16_t low = test->count && test->low < watts ? test->low : watts;
    const unsigned tolerance = high / 8 > 20 ? high / 8 : 20;
    if (!test->count || (unsigned)(high - low) > tolerance) {
        test->since = now;
        test->sum = watts;
        test->low = test->high = watts;
        test->count = 1;
        return false;
    }
    test->high = high;
    test->low = low;
    test->sum += watts;
    ++test->count;
    if (test->count < 3 || now - test->since < stable_seconds) return false;
    *result = (dispatch_power_test_result){test->peak, (test->sum + test->count / 2) / test->count};
    return true;
}

void dispatch_profile_sample(dispatch_profile *p, double mhs, bool power_known, double watts)
{
    if (!p->hash_samples || mhs < p->min_mhs) p->min_mhs = mhs;
    if (mhs > p->max_mhs) p->max_mhs = mhs;
    p->mean_mhs += (mhs - p->mean_mhs) / sample_weight(&p->hash_samples);
    if (power_known) {
        if (!p->power_samples || watts < p->min_watts) p->min_watts = watts;
        if (watts > p->max_watts) p->max_watts = watts;
        p->mean_watts += (watts - p->mean_watts) / sample_weight(&p->power_samples);
    }
}

void dispatch_profile_start(dispatch_profile *p, uint32_t seconds)
{
    p->startup_seconds = seconds;
    if (p->startups < UINT16_MAX) {
        ++p->startups;
    }
}

bool dispatch_efficiency_sample(dispatch_efficiency *efficiency, double watts, double mhs)
{
    if (!(watts > 0) || !(mhs > 0)) return false;
    const double j_per_gh = watts / (mhs / 1000.0);
    if (!(j_per_gh > 0) || !isfinite(j_per_gh)) return false;
    efficiency->mean_j_per_gh += (j_per_gh - efficiency->mean_j_per_gh) /
        sample_weight(&efficiency->samples);
    return true;
}

bool dispatch_preset_sample(dispatch_preset_totals *totals, uint32_t seconds, double watts, double mhs, double peak_watts)
{
    if (!seconds || !(watts > 0 && watts <= 50000) || !(mhs > 0 && mhs <= 1e12) ||
        !(peak_watts >= watts && peak_watts <= 50000)) return false;
    const dispatch_preset_totals period = {
        .joules = watts * seconds,
        .terahashes = mhs / 1e6 * seconds,
        .seconds = seconds,
        .peak_watts = (float)peak_watts,
    };
    dispatch_preset_merge(totals, &period);
    return true;
}

void dispatch_preset_merge(dispatch_preset_totals *totals, const dispatch_preset_totals *period)
{
    if (UINT32_MAX - totals->seconds < period->seconds) return;
    totals->joules += period->joules;
    totals->terahashes += period->terahashes;
    totals->seconds += period->seconds;
    if (period->peak_watts > totals->peak_watts) totals->peak_watts = period->peak_watts;
}

double dispatch_preset_efficiency(const dispatch_preset_totals *totals)
{
    return totals->terahashes > 0 ? totals->joules / totals->terahashes : 0;
}

void dispatch_preset_record_window(dispatch_preset_evidence *evidence, const dispatch_preset_totals *window)
{
    if (window->seconds < DISPATCH_PRESET_WINDOW_SECONDS || !window->terahashes) return;
    evidence->windows[evidence->next_window] = (float)dispatch_preset_efficiency(window);
    evidence->next_window = (evidence->next_window + 1) % DISPATCH_PRESET_WINDOWS;
}

unsigned dispatch_preset_window_count(const dispatch_preset_evidence *evidence)
{
    unsigned count = 0;
    for (unsigned i = 0; i < DISPATCH_PRESET_WINDOWS; ++i) count += evidence->windows[i] > 0;
    return count;
}

dispatch_preset_comparison dispatch_preset_compare(const dispatch_preset_evidence *normal, const dispatch_preset_evidence *reduced)
{
    if (dispatch_preset_window_count(normal) < DISPATCH_PRESET_WINDOWS ||
        dispatch_preset_window_count(reduced) < DISPATCH_PRESET_WINDOWS) return DISPATCH_PRESET_LEARNING;

    float normal_low = normal->windows[0], normal_high = normal_low;
    float reduced_low = reduced->windows[0], reduced_high = reduced_low;
    for (unsigned i = 1; i < DISPATCH_PRESET_WINDOWS; ++i) {
        if (normal->windows[i] < normal_low) normal_low = normal->windows[i];
        if (normal->windows[i] > normal_high) normal_high = normal->windows[i];
        if (reduced->windows[i] < reduced_low) reduced_low = reduced->windows[i];
        if (reduced->windows[i] > reduced_high) reduced_high = reduced->windows[i];
    }
    // Require the measured averages and even the least efficient recent window to agree.
    const double normal_average = dispatch_preset_efficiency(&normal->totals);
    const double reduced_average = dispatch_preset_efficiency(&reduced->totals);
    if (!(normal_average > 0 && reduced_average > 0)) return DISPATCH_PRESET_LEARNING;
    if (normal_high <= reduced_low * 0.95f && normal_average <= reduced_average * 0.95)
        return DISPATCH_PRESET_DEFAULT;
    if (reduced_high <= normal_low * 0.95f && reduced_average <= normal_average * 0.95)
        return DISPATCH_PRESET_REDUCED;
    return DISPATCH_PRESET_SIMILAR;
}

dispatch_work_change dispatch_work_update(dispatch_work_monitor *monitor, dispatch_work evidence, uint32_t now)
{
    enum { MISSING_SECONDS = 120, RESTORED_SECONDS = 30 };
    if (monitor->evidence != evidence) {
        monitor->evidence = evidence;
        monitor->since = now;
    }
    const uint32_t elapsed = now - monitor->since;
    if (evidence == DISPATCH_WORK_IDLE) {
        monitor->active = false;
    } else if (!monitor->active && evidence == DISPATCH_WORK_MISSING && elapsed >= MISSING_SECONDS) {
        monitor->active = true;
        return DISPATCH_WORK_LOST;
    } else if (monitor->active && evidence == DISPATCH_WORK_PRESENT && elapsed >= RESTORED_SECONDS) {
        monitor->active = false;
        return DISPATCH_WORK_RESTORED;
    }
    return DISPATCH_WORK_UNCHANGED;
}
