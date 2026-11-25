// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "dispatch_policy.h"

#include <stddef.h>

static bool in_window(uint32_t minute, uint16_t start, uint16_t end)
{
    return start <= end ? minute >= start && minute < end : minute >= start || minute < end;
}

bool dispatch_quiet_active(const dispatch_inputs *in)
{
    return in->clock_valid && in->quiet.enabled &&
        in_window(in->minute, in->quiet.start_minute, in->quiet.end_minute);
}

int64_t dispatch_solar_headroom(const dispatch_generation *solar)
{
    const uint32_t budget = solar->supported_production > solar->reserve ? solar->supported_production - solar->reserve : 0;
    return (int64_t)budget - solar->running_watts - solar->committed_watts;
}

bool dispatch_solar_deficit(const dispatch_generation *solar)
{
    return solar->configured && solar->fresh && solar->load_known &&
        dispatch_solar_coverage(solar->production, solar->reserve, solar->running_watts) < solar->running_watts;
}

uint32_t dispatch_solar_coverage(uint32_t production, uint32_t reserve, uint32_t draw)
{
    const uint32_t available = production > reserve ? production - reserve : 0;
    return available < draw ? available : draw;
}

uint32_t dispatch_solar_restart_remaining(const dispatch_inputs *in, unsigned i)
{
    const dispatch_outlet *outlet = &in->outlet[i];
    if (!in->solar.configured || !outlet->present || !outlet->fresh || outlet->on ||
        !in->solar_off_since[i]) return 0;
    const uint32_t lockout = in->solar.restart_seconds;
    const uint32_t elapsed = in->now - in->solar_off_since[i];
    return elapsed < lockout ? lockout - elapsed : 0;
}

static dispatch_reason generation_start(const dispatch_generation *solar, unsigned i, bool settled)
{
    if (solar->production <= solar->reserve) return DISPATCH_SOLAR_WAIT;
    if (!solar->load_known) return DISPATCH_SOLAR_LOAD_UNKNOWN;
    if (!solar->sustained) return DISPATCH_SOLAR_STABILIZING;
    const unsigned watts = solar->start_watts[i];
    if (!watts) return DISPATCH_SURPLUS_POWER;
    const int64_t available = dispatch_solar_headroom(solar) + ((solar->reserved_start_mask & (1u << i)) ? watts : 0);
    if (available < watts) return DISPATCH_SOLAR_WAIT;
    return settled ? DISPATCH_WINDOW_OPEN : DISPATCH_SOLAR_SETTLING;
}

bool dispatch_paid_covering(dispatch_reason permission)
{
    return permission == DISPATCH_WINDOW_OPEN || permission == DISPATCH_DAYLIGHT_BRIDGE;
}

dispatch_reason dispatch_operation_limit(const dispatch_inputs *in)
{
    if (in->paused) return DISPATCH_PAUSED;
    if (!in->clock_valid) return DISPATCH_CLOCK;
    if (dispatch_quiet_active(in)) return DISPATCH_QUIET_HOURS;
    if (in->policy.start_minute != in->policy.end_minute &&
        !in_window(in->minute, in->policy.start_minute, in->policy.end_minute)) return DISPATCH_WINDOW_CLOSED;
    return DISPATCH_WINDOW_OPEN;
}

static dispatch_reason paid_budget(const dispatch_inputs *in, unsigned start, uint32_t extra_watts)
{
    const uint64_t limit = (uint64_t)in->policy.daily_allowance_millicents * 1000000;
    if (!limit) return DISPATCH_WINDOW_OPEN;
    if (!in->accounting_valid) return DISPATCH_BUDGET_UNKNOWN;
    if (in->used_nano_cents >= limit) return DISPATCH_BUDGET_USED;
    uint32_t watts = in->solar.running_watts + in->solar.committed_watts;
    if (!in->solar.load_known) watts = DISPATCH_LIMIT * DISPATCH_METER_CHANNEL_WATTS;
    if (start < DISPATCH_LIMIT && !(in->solar.reserved_start_mask & (1u << start))) {
        watts += in->solar.start_watts[start];
    }
    watts += extra_watts;
    // Cover a full minimum-on interval for a new start, plus polling and sequential relay commands.
    const uint32_t seconds = 20 * DISPATCH_LIMIT + (start < DISPATCH_LIMIT ? DISPATCH_MIN_ON : 0);
    const uint64_t milliwh = ((uint64_t)watts * seconds * 1000 + 3599) / 3600;
    return milliwh * in->guard_rate <= limit - in->used_nano_cents ? DISPATCH_WINDOW_OPEN : DISPATCH_PAID_RESERVE;
}

bool dispatch_paid_room(const dispatch_inputs *in, uint32_t extra_watts)
{
    return paid_budget(in, DISPATCH_LIMIT, extra_watts) == DISPATCH_WINDOW_OPEN;
}

dispatch_reason dispatch_bridge_permission(const dispatch_inputs *in)
{
    const dispatch_reason limit = dispatch_operation_limit(in);
    if (limit != DISPATCH_WINDOW_OPEN) return limit;
    const dispatch_generation *solar = &in->solar;
    const dispatch_daylight_bridge *bridge = &solar->bridge;
    if (!bridge->enabled) return DISPATCH_PAID_DISABLED;
    if (!bridge->daylight) return DISPATCH_DAYLIGHT_DARK;
    if (!solar->fresh || !solar->load_known ||
        (in->grid_source && !in->grid_recent_export && !bridge->recent_solar)) return DISPATCH_DAYLIGHT_EVIDENCE;
    if (!dispatch_solar_deficit(solar)) return DISPATCH_DAYLIGHT_WAIT;
    if (!bridge->seconds) return DISPATCH_DAYLIGHT_EXPIRED;
    const dispatch_reason budget = paid_budget(in, DISPATCH_LIMIT, 0);
    if (budget == DISPATCH_PAID_RESERVE) return DISPATCH_DAYLIGHT_BUDGET;
    return budget == DISPATCH_WINDOW_OPEN ? DISPATCH_DAYLIGHT_BRIDGE : budget;
}

dispatch_reason dispatch_paid_permission(const dispatch_inputs *in)
{
    const dispatch_reason limit = dispatch_operation_limit(in);
    if (limit != DISPATCH_WINDOW_OPEN) return limit;
    const dispatch_operating_plan *plan = &in->operating;
    if (plan->paid_enabled && (plan->paid_start == plan->paid_end ||
        in_window(in->minute, plan->paid_start, plan->paid_end))) return paid_budget(in, DISPATCH_LIMIT, 0);
    return in->solar.bridge.enabled ? dispatch_bridge_permission(in) :
        plan->paid_enabled ? DISPATCH_WINDOW_CLOSED : DISPATCH_PAID_DISABLED;
}

dispatch_gap_forecast dispatch_daylight_forecast(const dispatch_inputs *in, uint32_t savings_watts)
{
    const dispatch_generation *solar = &in->solar;
    const uint64_t limit = (uint64_t)in->policy.daily_allowance_millicents * 1000000;
    const uint32_t rate = solar->bridge.forecast_rate;
    dispatch_gap_forecast result = {.seconds_to_sunset = solar->bridge.seconds_to_sunset};
    if (!result.seconds_to_sunset || !rate || !limit || !in->accounting_valid ||
        !solar->fresh || !solar->load_known) return result;

    const uint32_t covered = dispatch_solar_coverage(solar->production, solar->reserve, solar->running_watts);
    const uint32_t paid = solar->running_watts - covered;
    result.known = true;
    result.paid_watts = paid > savings_watts ? paid - savings_watts : 0;
    const uint64_t remaining = limit > in->used_nano_cents ? limit - in->used_nano_cents : 0;
    const uint64_t seconds = result.paid_watts ?
        remaining * 3600 / ((uint64_t)rate * result.paid_watts * 1000) : UINT32_MAX;
    result.runway_seconds = seconds < UINT32_MAX ? seconds : UINT32_MAX;
    return result;
}

static bool shed_before(const dispatch_inputs *in, unsigned slot, unsigned prior)
{
    if (prior == DISPATCH_LIMIT) return true;
    if (in->priority[slot] != in->priority[prior]) return in->priority[slot] < in->priority[prior];
    const bool productive = in->productive_mask & (1u << slot);
    const bool prior_productive = in->productive_mask & (1u << prior);
    if (productive != prior_productive) return !productive;
    const int64_t shortfall = -dispatch_solar_headroom(&in->solar);
    const uint16_t draw = in->draw_watts[slot], prior_draw = in->draw_watts[prior];
    if (shortfall > 0 && draw >= shortfall && prior_draw >= shortfall) return draw < prior_draw;
    return draw > prior_draw;
}

static bool near_cutoff(uint32_t minute, uint32_t cutoff)
{
    const uint32_t remaining = (cutoff + 1440 - minute) % 1440;
    return remaining && remaining <= DISPATCH_MIN_ON / 60;
}

void dispatch_plan(const dispatch_inputs *in, dispatch_decision out[DISPATCH_LIMIT])
{
    const dispatch_generation *solar = &in->solar;
    const dispatch_reason allowed = dispatch_operation_limit(in);
    const dispatch_reason permission = dispatch_paid_permission(in);
    const bool paid = permission == DISPATCH_WINDOW_OPEN;
    const bool bridge = permission == DISPATCH_DAYLIGHT_BRIDGE;
    const bool source = solar->configured;
    const bool usable = source && solar->fresh && solar->load_known;
    const bool deficit = dispatch_solar_deficit(solar);
    const bool can_cover_transition = dispatch_paid_room(in, 0);
    const bool grace = source && !usable && in->source_grace && can_cover_transition;
    const bool delaying = usable && deficit && can_cover_transition &&
        (solar->shed_wait_seconds || solar->shed_needs_reading);
    const bool settled = in->now >= in->surplus.next_change;
    unsigned start = DISPATCH_LIMIT, shed = DISPATCH_LIMIT;
    dispatch_reason start_reason = DISPATCH_START_PENDING;
    bool stopping = false;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        const dispatch_outlet *outlet = &in->outlet[i];
        out[i] = (dispatch_decision){DISPATCH_KEEP, allowed};
        if (!outlet->present || !outlet->fresh) {
            out[i].reason = DISPATCH_UNOBSERVED;
            continue;
        }
        if (allowed == DISPATCH_CLOCK) continue;
        const uint8_t bit = 1u << i;
        if (allowed == DISPATCH_WINDOW_OPEN && (in->manual_hold_mask & bit))
            out[i].reason = DISPATCH_MANUAL_HOLD;
        if (allowed == DISPATCH_WINDOW_OPEN && !outlet->on && (in->recovery_hold_mask & bit))
            out[i].reason = DISPATCH_RECOVERY_HOLD;
        if (out[i].reason != DISPATCH_WINDOW_OPEN) {
            if (!outlet->on) continue;
            if (allowed == DISPATCH_WINDOW_OPEN && in->now - outlet->since < DISPATCH_MIN_ON) {
                out[i].reason = DISPATCH_DWELL;
                continue;
            }
            out[i].action = DISPATCH_OFF;
            stopping = true;
            continue;
        }
        if (outlet->on) {
            if (usable && !deficit) out[i].reason = DISPATCH_SOLAR_COVERED;
            else if (paid) out[i].reason = DISPATCH_PAID_COVERAGE;
            else if (bridge) out[i].reason = DISPATCH_DAYLIGHT_BRIDGE;
            else if (grace) out[i].reason = DISPATCH_SOURCE_GRACE;
            else if (delaying && permission != DISPATCH_DAYLIGHT_EXPIRED && permission != DISPATCH_DAYLIGHT_DARK)
                out[i].reason = DISPATCH_SOLAR_SHED_DELAY;
            else {
                out[i].reason = usable && deficit &&
                    (permission == DISPATCH_PAID_DISABLED || permission == DISPATCH_WINDOW_CLOSED ||
                     permission == DISPATCH_DAYLIGHT_WAIT) ? DISPATCH_SOLAR_DEFICIT_PENDING : permission;
                if (settled && in->now - outlet->since >= DISPATCH_MIN_ON && shed_before(in, i, shed)) shed = i;
            }
            continue;
        }
        dispatch_reason renewable = usable ? generation_start(solar, i, settled && !deficit) :
            source ? (solar->fresh ? DISPATCH_SOLAR_LOAD_UNKNOWN : DISPATCH_SOLAR_STALE) : DISPATCH_PAID_DISABLED;
        if (renewable == DISPATCH_WINDOW_OPEN && in->grid_source &&
            -(int64_t)in->surplus.grid_watts < (int64_t)solar->start_watts[i] + in->surplus.start_reserve &&
            !(solar->reserved_start_mask & bit)) renewable = DISPATCH_SURPLUS_WAIT;
        if (!solar->start_watts[i]) out[i].reason = DISPATCH_SURPLUS_POWER;
        else if (renewable != DISPATCH_WINDOW_OPEN && !paid) {
            out[i].reason = bridge ? DISPATCH_DAYLIGHT_WAIT :
                usable && (!deficit || permission == DISPATCH_PAID_DISABLED || permission == DISPATCH_WINDOW_CLOSED ||
                           permission == DISPATCH_DAYLIGHT_WAIT) ? renewable :
                !usable && source && (permission == DISPATCH_PAID_DISABLED || permission == DISPATCH_WINDOW_CLOSED) ?
                    renewable : permission;
        } else if ((in->policy.start_minute != in->policy.end_minute &&
                    near_cutoff(in->minute, in->policy.end_minute)) ||
                   (in->quiet.enabled && near_cutoff(in->minute, in->quiet.start_minute)) ||
                   (renewable != DISPATCH_WINDOW_OPEN && in->operating.paid_start != in->operating.paid_end &&
                    near_cutoff(in->minute, in->operating.paid_end))) out[i].reason = DISPATCH_START_WINDOW_SHORT;
        else if (dispatch_solar_restart_remaining(in, i)) out[i].reason = DISPATCH_SOLAR_RESTART;
        else if (in->now - outlet->since < DISPATCH_MIN_OFF) out[i].reason = DISPATCH_DWELL;
        else if (renewable != DISPATCH_WINDOW_OPEN &&
                 (out[i].reason = paid_budget(in, i, 0)) != DISPATCH_WINDOW_OPEN) continue;
        else {
            out[i].reason = DISPATCH_START_PENDING;
            if (start == DISPATCH_LIMIT || in->priority[i] > in->priority[start] ||
                (in->priority[i] == in->priority[start] &&
                 (renewable == DISPATCH_WINDOW_OPEN ? start_reason == DISPATCH_PAID_START ||
                  solar->start_watts[i] < solar->start_watts[start] :
                  start_reason == DISPATCH_PAID_START && solar->start_watts[i] < solar->start_watts[start]))) {
                start = i;
                start_reason = renewable == DISPATCH_WINDOW_OPEN ?
                    (in->grid_source ? DISPATCH_SURPLUS_START : DISPATCH_SOLAR_START) : DISPATCH_PAID_START;
            }
        }
    }
    if (shed != DISPATCH_LIMIT && !stopping) {
        const dispatch_reason why = out[shed].reason;
        out[shed] = (dispatch_decision){DISPATCH_OFF, why == DISPATCH_SOLAR_DEFICIT_PENDING ? DISPATCH_SOLAR_STOP :
            !usable && source && (why == DISPATCH_WINDOW_CLOSED || why == DISPATCH_PAID_DISABLED) ?
                DISPATCH_SOLAR_OR_PAID_REQUIRED : why};
    } else if (!stopping && start != DISPATCH_LIMIT && in->now >= in->next_start && settled) {
        out[start] = (dispatch_decision){DISPATCH_ON, start_reason};
    }
}

dispatch_solar_preset_decision dispatch_solar_choose_preset(const dispatch_inputs *in,
    const dispatch_decision decisions[DISPATCH_LIMIT], const dispatch_solar_preset_slot slots[DISPATCH_LIMIT],
    bool confirmed_shortfall)
{
    const dispatch_generation *solar = &in->solar;
    dispatch_solar_preset_decision chosen = {.slot = DISPATCH_LIMIT};
    if (!solar->configured || !solar->fresh || !solar->load_known || !solar->sustained ||
        dispatch_operation_limit(in) != DISPATCH_WINDOW_OPEN) return chosen;

    const dispatch_reason permission = dispatch_paid_permission(in);
    const bool bridging = permission == DISPATCH_DAYLIGHT_BRIDGE;
    const bool paid = permission == DISPATCH_WINDOW_OPEN;
    const int64_t headroom = dispatch_solar_headroom(solar);
    const dispatch_gap_forecast forecast = bridging ? dispatch_daylight_forecast(in, 0) : (dispatch_gap_forecast){0};
    const bool funded_until_sunset = forecast.known && forecast.runway_seconds >= forecast.seconds_to_sunset;
    const bool lowering = bridging ? confirmed_shortfall && dispatch_solar_deficit(solar) :
        headroom < 0 && confirmed_shortfall;
    if (bridging && (!lowering || solar->bridge.seconds < DISPATCH_BRIDGE_PRESET_MIN_SECONDS)) return chosen;
    if (headroom < 0 && !lowering) return chosen;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        const dispatch_decision decision = decisions[i];
        // If another control constraint is stopping a miner, let the outlet decision finish first.
        if (decision.action == DISPATCH_OFF && (bridging || !lowering || decision.reason != DISPATCH_SOLAR_STOP))
            return (dispatch_solar_preset_decision){.slot = DISPATCH_LIMIT};
        if (decision.action == DISPATCH_ON && (!lowering || decision.reason != DISPATCH_PAID_START))
            return (dispatch_solar_preset_decision){.slot = DISPATCH_LIMIT};
    }
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        const dispatch_solar_preset_slot *slot = &slots[i];
        if (!slot->ready || !in->outlet[i].fresh || !in->outlet[i].on ||
            ((in->manual_hold_mask | in->recovery_hold_mask) & (1u << i))) continue;
        const unsigned prior = chosen.slot;
        if (lowering) {
            if (slot->reduced || !slot->reduced_watts || slot->draw <= slot->reduced_watts) continue;
            const uint32_t saving = slot->draw - slot->reduced_watts;
            if ((bridging && (!slot->bridge_efficient || (funded_until_sunset && saving < forecast.paid_watts))) ||
                (paid && (saving < 25 || (uint64_t)saving * 20 < slot->draw)) ||
                (!bridging && !paid && headroom + saving < 0)) continue;
            if (prior != DISPATCH_LIMIT && (in->priority[prior] < in->priority[i] ||
                (in->priority[prior] == in->priority[i] && slots[prior].draw - slots[prior].reduced_watts >= saving))) continue;
            chosen = (dispatch_solar_preset_decision){bridging ? DISPATCH_PRESET_BRIDGE_LOWER : DISPATCH_PRESET_LOWER, i, 0};
        } else {
            if (!slot->reduced || slot->default_watts <= slot->draw ||
                (prior != DISPATCH_LIMIT && in->priority[prior] >= in->priority[i])) continue;
            const uint32_t extra = slot->default_watts - slot->draw;
            uint32_t margin = solar->shed_margin > 50 ? solar->shed_margin : 50;
            if (in->grid_source && in->surplus.start_reserve > margin) margin = in->surplus.start_reserve;
            if (headroom < (int64_t)extra + margin) continue;
            chosen = (dispatch_solar_preset_decision){DISPATCH_PRESET_RESTORE, i, extra};
        }
    }
    return chosen;
}

const char *dispatch_explanation(dispatch_reason reason)
{
    static const char *const messages[] = {
        [DISPATCH_UNOBSERVED] = "Waiting for a fresh outlet observation",
        [DISPATCH_PAUSED] = "Temporary pause is active",
        [DISPATCH_CLOCK] = "Waiting for a synchronized clock",
        [DISPATCH_WINDOW_CLOSED] = "Outside the allowed schedule",
        [DISPATCH_BUDGET_UNKNOWN] = "Waiting for complete electricity accounting",
        [DISPATCH_BUDGET_USED] = "Daily electricity allowance reached",
        [DISPATCH_WINDOW_OPEN] = "Schedule and allowance permit operation",
        [DISPATCH_DWELL] = "Waiting for the minimum run or off time",
        [DISPATCH_START_PENDING] = "Waiting for staggered startup",
        [DISPATCH_COMMAND_HOLD] = "Command unconfirmed after retries. Reopen Preview to rearm",
        [DISPATCH_QUIET_HOURS] = "Quiet Hours are active",
        [DISPATCH_QUIET_DWELL] = "Quiet Hours are active. Waiting for minimum run time",
        [DISPATCH_MANUAL_HOLD] = "Owner placed this miner on a temporary hold",
        [DISPATCH_RECOVERY_HOLD] = "Waiting for the recovery off period",
        [DISPATCH_RECOVERY] = "Sustained zero hashrate with a healthy mining path. Recovery eligible",
        [DISPATCH_SURPLUS_STALE] = "Grid meter unavailable. Holding outlet state",
        [DISPATCH_SURPLUS_WAIT] = "Waiting for available export and the settling interval",
        [DISPATCH_SURPLUS_POWER] = "Startup draw is unknown. Run a first-run power test or enter an estimate.",
        [DISPATCH_SURPLUS_START] = "Measured export covers this miner and the reserve",
        [DISPATCH_SURPLUS_STOP] = "Sustained grid import. Shedding a lower-priority miner",
        [DISPATCH_CONFIRM_MAPPING] = "Confirm which miner this outlet powers before switching",
        [DISPATCH_SOLAR_STALE] = "Waiting for a fresh energy reading before starting",
        [DISPATCH_SOLAR_STALE_STOP] = "ECU production unavailable. Shedding under the selected fallback",
        [DISPATCH_SOLAR_STABILIZING] = "Waiting for two usable energy readings",
        [DISPATCH_SOLAR_LOAD_UNKNOWN] = "Waiting for fresh miner outlet readings",
        [DISPATCH_SOLAR_WAIT] = "Waiting for enough estimated solar power to start",
        [DISPATCH_SOLAR_SETTLING] = "Waiting for outlet power to settle before a solar start",
        [DISPATCH_SOLAR_START] = "Estimated solar headroom covers this miner's startup draw",
        [DISPATCH_SOLAR_STOP] = "Miner draw exceeds the estimated solar budget. Shedding a lower-priority miner",
        [DISPATCH_SOLAR_RESTART] = "Waiting for the outlet restart lockout",
        [DISPATCH_SOLAR_SHED_DELAY] = "Low solar production. Waiting for the shedding delay and confirmation",
        [DISPATCH_SOLAR_COVERED] = "Estimated solar covers the running miner draw",
        [DISPATCH_PAID_COVERAGE] = "The paid window and budget can cover any solar shortfall",
        [DISPATCH_PAID_START] = "The paid window and budget can cover startup and any solar shortfall",
        [DISPATCH_SOLAR_OR_PAID_REQUIRED] = "Solar coverage is unavailable and paid electricity is blocked",
        [DISPATCH_SOLAR_DEFICIT_PENDING] = "Solar falls short. Waiting for an eligible lower-priority outlet to shed",
        [DISPATCH_DAYLIGHT_WAIT] = "Daylight coverage is on standby. Starts require sufficient solar",
        [DISPATCH_DAYLIGHT_DARK] = "Outside daylight gap coverage hours",
        [DISPATCH_DAYLIGHT_BRIDGE] = "Paid electricity is keeping the running miner on through a daylight gap",
        [DISPATCH_DAYLIGHT_EXPIRED] = "Daylight gap coverage reached its time limit",
        [DISPATCH_DAYLIGHT_BUDGET] = "Too little paid allowance remains to bridge this gap",
        [DISPATCH_DAYLIGHT_EVIDENCE] = "Daylight gap coverage needs recent solar and outlet readings",
        [DISPATCH_POWER_TEST] = "This outlet is reserved for the owner-approved first-run power test",
        [DISPATCH_PAID_DISABLED] = "Your plan does not allow paid mining",
        [DISPATCH_PAID_RESERVE] = "The remaining paid allowance cannot cover this action",
        [DISPATCH_SOURCE_GRACE] = "Energy readings are unavailable. Briefly holding the running outlet while the source recovers",
        [DISPATCH_START_WINDOW_SHORT] = "Too little time remains before the next stop to start this miner",
    };
    return messages[reason];
}

bool dispatch_tariff_valid(const dispatch_tariff *tariff)
{
    for (unsigned i = 0; i < DISPATCH_RATE_WINDOWS; ++i) {
        const dispatch_rate_window *window = &tariff->window[i];
        if (window->start_minute >= 1440 || window->end_minute >= 1440 ||
            window->rate_millicents_per_kwh > DISPATCH_RATE_MAX ||
            (window->enabled && window->start_minute == window->end_minute)) {
            return false;
        }
        if (!window->enabled) {
            continue;
        }
        for (unsigned j = 0; j < i; ++j) {
            const dispatch_rate_window *other = &tariff->window[j];
            if (other->enabled &&
                (in_window(window->start_minute, other->start_minute, other->end_minute) ||
                 in_window(other->start_minute, window->start_minute, window->end_minute))) {
                return false;
            }
        }
    }
    return true;
}

uint32_t dispatch_highest_rate(uint32_t base, const dispatch_tariff *tariff)
{
    uint32_t highest = base;
    for (unsigned i = 0; tariff && i < DISPATCH_RATE_WINDOWS; ++i) {
        const dispatch_rate_window *window = &tariff->window[i];
        if (window->enabled && window->rate_millicents_per_kwh > highest) {
            highest = window->rate_millicents_per_kwh;
        }
    }
    return highest;
}

uint32_t dispatch_rate_at(uint32_t base, const dispatch_tariff *tariff, uint32_t minute, unsigned *active_window)
{
    for (unsigned i = 0; i < DISPATCH_RATE_WINDOWS; ++i) {
        const dispatch_rate_window *window = &tariff->window[i];
        if (window->enabled && in_window(minute, window->start_minute, window->end_minute)) {
            if (active_window) {
                *active_window = i + 1;
            }
            return window->rate_millicents_per_kwh;
        }
    }
    if (active_window) {
        *active_window = 0;
    }
    return base;
}

bool dispatch_is_weekend(uint32_t local_day)
{
    // January 1, 1970 was Thursday. Local day numbers are civil days since that date.
    const unsigned weekday = (local_day % 7 + 4) % 7;
    return weekday == 0 || weekday == 6;
}

void dispatch_apply_weekend(dispatch_inputs *input, const dispatch_weekend_plan *weekend, uint32_t local_day)
{
    if (!weekend->enabled || !dispatch_is_weekend(local_day)) return;
    input->policy.start_minute = weekend->start_minute;
    input->policy.end_minute = weekend->end_minute;
    input->quiet = weekend->quiet;
    input->operating.paid_start = input->operating.weekend_paid_start;
    input->operating.paid_end = input->operating.weekend_paid_end;
}

uint32_t dispatch_meter_rate(uint32_t base, const dispatch_tariff *tariff,
                             const dispatch_tariff *weekend, uint64_t from, uint64_t to,
                             dispatch_local_time local, bool *used_highest)
{
    *used_highest = false;
    if (!from || to < from) {
        return base;
    }
    // Controller timezone transitions and rate boundaries are aligned to UTC minutes.
    // Walk at most six slices. The meter reports one total for this entire interval.
    const uint64_t duration = to - from;
    uint32_t day, minute;
    if (!duration && local(to, &day, &minute)) {
        return dispatch_rate_at(base, weekend && dispatch_is_weekend(day) ? weekend : tariff, minute, NULL);
    }
    if (duration && duration <= DISPATCH_PRORATE_SECONDS) {
        uint32_t weighted = 0;
        for (uint64_t begin = from; begin < to;) {
            if (!local(begin, &day, &minute)) {
                goto conservative;
            }
            uint32_t seconds = 60 - begin % 60;
            if (seconds > to - begin) {
                seconds = to - begin;
            }
            const dispatch_tariff *active = weekend && dispatch_is_weekend(day) ? weekend : tariff;
            weighted += seconds * dispatch_rate_at(base, active, minute, NULL);
            begin += seconds;
        }
        return (weighted + duration - 1) / duration;
    }
conservative:
    *used_highest = true;
    return dispatch_highest_rate(dispatch_highest_rate(base, tariff), weekend);
}

bool dispatch_budget_day(dispatch_budget *budget, uint32_t local_day)
{
    if (!local_day || local_day < budget->day) {
        return false;
    }
    if (local_day > budget->day) {
        budget->day = local_day;
        budget->used_nano_cents = 0;
        budget->gap_mask = 0;
    }
    return true;
}

void dispatch_budget_carry(dispatch_budget *budget, uint32_t local_day, uint64_t recorded_nano_cents)
{
    budget->day = local_day;
    if (recorded_nano_cents > budget->used_nano_cents) {
        budget->used_nano_cents = recorded_nano_cents;
    }
}

uint64_t dispatch_budget_sample(dispatch_budget *budget, unsigned outlet,
                            uint64_t total_mwh, uint64_t epoch, uint32_t rate)
{
    return dispatch_budget_split_sample(budget, outlet, total_mwh, epoch, rate, 0).metered_mwh;
}

dispatch_energy_sample dispatch_budget_split_sample(dispatch_budget *budget, unsigned outlet,
    uint64_t total_mwh, uint64_t epoch, uint32_t rate, uint64_t available_solar_mwh)
{
    dispatch_energy_sample accepted = {0};
    if (outlet >= DISPATCH_LIMIT || !epoch || !budget->day) {
        return accepted;
    }
    dispatch_meter_anchor *anchor = &budget->anchor[outlet];
    if (anchor->epoch) {
        uint64_t elapsed = epoch > anchor->epoch ? epoch - anchor->epoch : 0;
        if (epoch < anchor->epoch || total_mwh < anchor->total_mwh ||
            elapsed > UINT32_MAX) {
            budget->gap_mask |= 1u << outlet;
        } else {
            const uint64_t delta = total_mwh - anchor->total_mwh;
            const uint64_t solar = elapsed && elapsed <= DISPATCH_PRORATE_SECONDS ?
                (available_solar_mwh < delta ? available_solar_mwh : delta) : 0;
            const uint64_t paid = delta - solar;
            // Allow 10 Wh for device update granularity and clock correction.
            const uint64_t maximum = 10000 + elapsed * DISPATCH_METER_CHANNEL_WATTS * 1000 / 3600;
            if (delta > maximum || (rate && paid > (UINT64_MAX - budget->used_nano_cents) / rate)) {
                budget->gap_mask |= 1u << outlet;
            } else {
                budget->used_nano_cents += paid * rate;
                accepted = (dispatch_energy_sample){delta, solar};
            }
        }
    }
    *anchor = (dispatch_meter_anchor){total_mwh, epoch};
    return accepted;
}
