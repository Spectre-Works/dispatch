// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0

// Compile the installation worker twice with its two production admission
// modes. Both fixtures link observation-only adapters and use synthetic time.
#ifdef REPLAY_ATOM_WORKER
#undef DISPATCH_PREVIEW_ONLY
#endif
#include "freertos/task.h"
static int replay_task(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
#define xTaskCreate replay_task
#define network_clock replay_clock
#define network_uptime replay_uptime
#include "installation.c"
#include "platform.h"
#include <assert.h>

static const time_t epoch = 1795521600;
static uint32_t elapsed;
static installation_view view;

static int replay_task(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{
    return pdPASS;
}

time_t replay_clock(void) { return epoch + elapsed; }
uint32_t replay_uptime(void) { return elapsed; }

static void sample(uint32_t when, double watts, uint64_t total)
{
    elapsed = when;
    const shelly_sample outlet = {.on = true, .power_known = true, .energy_known = true,
        .watts = watts, .total_mwh = total};
    observation(0, SHELLY_OK, &outlet, NULL, true, when);
}

static void grid_sample(uint32_t when, int32_t watts)
{
    grid.previous_watts = grid.watts;
    grid.watts = watts;
    grid.at = when;
    grid.epoch = network_epoch();
    grid.result = SHELLY_OK;
    grid.attempted = grid.sampled = grid.consecutive = true;
    grid.importing = watts > 0;
}

static void output(const char *step)
{
    installation_read(&view);
    const device_view *device = &view.device[0];
    assert(!view.enabled && !device->command_pending && device->power_test.phase == POWER_TEST_IDLE);
    printf("%s outlet=%d solar=%d/%u grid=%d/%ld action=%d reason=%s cost=%llu complete=%d gap=%d\n",
        step, device->observed, view.energy.solar_fresh, view.energy.production.watts,
        view.energy.grid_fresh, (long)view.energy.grid_watts, device->decision.action,
        dispatch_explanation(device->decision.reason), (unsigned long long)view.used_nano_cents,
        view.accounting_complete, view.meter_gap);
}

static void replay(bool restart)
{
    if (!restart) {
        state.settings.device[0] = (association){.shelly = {
            .ip = "10.241.0.1", .identity = "shellyplusplugsus-000000000001"}};
        state.first_run[0] = RUN_MAPPING;
        state.plan_ready = true;
        state.settings.policy.daily_allowance_millicents = 50000;
        state.solar = (solar_settings){.ip = "10.241.0.1", .port = 8899};
        state.grid.target = (shelly_target){.ip = "10.241.0.2", .identity = "fronius-solar-api"};
        state.grid.direction_verified = true;
        state.follow.shed_delay_seconds = 0;
        state.follow.shed_margin = 0;
        solar.reading.watts = solar.supported_watts = 1300;
        solar.epoch = network_epoch();
        solar.at = 5;
        solar.result = ECU_OK;
        solar.sampled = solar.sustained = true;
        grid_sample(5, -600);
        sample(5, 500, 1000000);
        for (uint32_t at = 10; at <= 130; at += 5) {
            grid_sample(at, -600);
            sample(at, 500, 1000000 + (at - 5) * 140);
        }
        output("surplus");
        assert(view.energy.solar_fresh && view.energy.grid_fresh && view.energy.grid_watts == -600);
        solar.reading.watts = solar.supported_watts = 100;
        solar.at = 135;
        grid.import_since = 135;
        for (uint32_t at = 135; at <= 165; at += 5) {
            grid_sample(at, 450);
            sample(at, 500, 1000000 + (at - 5) * 140);
        }
        output("shortfall");
        assert(view.energy.production.watts == 100 && view.energy.grid_watts == 450);
        assert(view.device[0].decision.action == DISPATCH_OFF);
        elapsed = 201;
        output("stale");
        assert(!view.energy.grid_fresh && !view.device[0].observed);
        observation(0, SHELLY_NETWORK, &(shelly_sample){0}, NULL, true, elapsed);
        grid_sample(210, -600);
        sample(210, 500, 1030000);
        output("recovered");
        assert(view.energy.grid_fresh && view.device[0].observed && view.used_nano_cents > 0);
        assert(save() == ESP_OK);
    } else {
        elapsed = 215;
        output("restart");
        assert(view.plan_ready && !view.energy.grid_fresh && !view.device[0].observed && view.used_nano_cents > 0);
        grid_sample(220, -600);
        sample(220, 500, 1031000);
        output("restart poll");
        assert(view.energy.grid_fresh && view.device[0].observed);
    }
}

static void archive_evening(unsigned ago, int shift, int missing_after_cutoff, bool rebound)
{
    const int32_t latitude = state.daylight.latitude_e4, longitude = state.daylight.longitude_e4;
    const dispatch_daylight sun = dispatch_sun_times(dispatch_solar_day(epoch, longitude) - ago, latitude, longitude);
    const unsigned period = timeline_period(1), ratio = period / TIMELINE_SECONDS;
    const uint32_t cutoff = (sun.sunset + shift + period / 2) / period;
    const uint32_t end = (sun.sunset + DISPATCH_SUNSET_MAX_SHIFT + 1800 + period - 1) / period;
    for (uint32_t row = (sun.sunset - DISPATCH_SUNSET_MAX_SHIFT) / period; row < end; ++row) {
        if (missing_after_cutoff >= 0 && row == cutoff + (uint32_t)missing_after_cutoff) continue;
        const uint16_t production = row < cutoff || (rebound && row == end - 1) ? 30 : 0;
        timeline_sample sample = {.solar_decawatts = production, .solar_min = production, .solar_max = production,
            .solar_count = 30, .grid_decawatts = TIMELINE_UNKNOWN_GRID};
        for (unsigned i = 0; i < ratio; ++i) timeline_sample_write(row * ratio + i, &sample);
        timeline_complete((row + 1) * ratio);
    }
}

static void learn_sunset(void)
{
    assert(state.daylight.enabled);
    // The worker reads at most one 15-minute row per pass.
    for (unsigned i = 0; i < 160; ++i) sunset_history_tick(epoch);
}

static void sunset_replay(bool restart)
{
    if (restart) {
        assert(state.solar_history_tag == SOLAR_HISTORY_TAG && state.solar_history_from == epoch);
        assert(!strcmp(state.solar.ip, "10.241.0.2"));
        learn_sunset();
        assert(!sunset_history.cutoff);
        return;
    }
    state.daylight = (daylight_settings){.latitude_e4 = 397392, .longitude_e4 = -1049903, .minutes = 30, .enabled = true};
    state.solar = (solar_settings){.ip = "10.241.0.1", .port = 8899};
    const dispatch_daylight calculated = daylight_at(epoch);
    archive_evening(6, -2400, -1, true);
    archive_evening(5, -2400, 0, false);
    archive_evening(4, -4800, -1, false);
    archive_evening(3, -2400, 3, false);
    archive_evening(2, -2400, -1, false);
    archive_evening(1, -2400, -1, false);
    learn_sunset();
    bool learned;
    dispatch_daylight adjusted = daylight_gap_at(epoch, &learned);
    assert(learned && sunset_history.count == 4 && adjusted.sunrise == calculated.sunrise);
    assert(adjusted.sunset >= calculated.sunset - 2850 && adjusted.sunset <= calculated.sunset - 1950);
    const dispatch_inputs before = inputs(0, adjusted.sunset - 90);
    assert(before.solar.bridge.daylight && before.solar.bridge.seconds == 90 && before.solar.bridge.seconds_to_sunset == 90);
    const dispatch_decision decisions[DISPATCH_LIMIT] = {0};
    const operation_next next = next_operation(&before, decisions, adjusted.sunset - 90);
    assert(next.kind == NEXT_SUNSET && next.seconds == 90);
    assert(!inputs(0, adjusted.sunset).solar.bridge.daylight);
    elapsed = adjusted.sunset - 90 - epoch;
    installation_read(&view);
    uint32_t minute;
    assert(local_minute(adjusted.sunset, &minute));
    assert(view.energy.sunset_learned && view.energy.sunset_minute == minute);

    elapsed = 0;
    assert(installation_accounting(ACCOUNTING_CLEAR_ACTIVITY) == ESP_OK);
    assert(daylight_gap_at(epoch, &learned).sunset == calculated.sunset && !learned);
    for (unsigned ago = 3; ago; --ago) archive_evening(ago, 2400, -1, false);
    learn_sunset();
    adjusted = daylight_gap_at(epoch, &learned);
    assert(learned && adjusted.sunset >= calculated.sunset + 1950 && adjusted.sunset <= calculated.sunset + 2850);
    assert(inputs(0, calculated.sunset).solar.bridge.daylight);
    assert(!inputs(0, adjusted.sunset).solar.bridge.daylight);

    solar_settings replacement = state.solar;
    strcpy(replacement.ip, "10.241.0.2");
    assert(installation_solar(&replacement) == ESP_OK);
    assert(daylight_gap_at(epoch, &learned).sunset == calculated.sunset && !learned);
    learn_sunset();
    assert(!sunset_history.cutoff);
    puts("Learned sunset drives daylight gap schedule, countdown and cutoff");
}

static void daylight_outlet_dropout(void)
{
    state.daylight = (daylight_settings){.latitude_e4 = 397392, .longitude_e4 = -1049903, .minutes = 30, .enabled = true};
    state.solar = (solar_settings){.ip = "10.241.0.1", .port = 8899};
    state.settings.policy.daily_allowance_millicents = 22000;
    state.follow.reserve = 500;
    state.follow.start_watts[2] = 200;
    state.enabled = true;
    for (unsigned i = 0; i < 3; ++i) state.first_run[i] = RUN_MAPPING;
    state.settings.device[0].shelly = (shelly_target){.ip = "10.241.0.1", .identity = "shellyplusplugsus-000000000001"};
    state.settings.device[1].shelly = (shelly_target){.ip = "10.241.0.2", .identity = "shellyplusplugsus-000000000002"};
    state.settings.device[2].shelly = (shelly_target){.ip = "10.241.0.3", .identity = "shellyplusplugsus-000000000003"};
    const dispatch_daylight sun = daylight_at(epoch);
    const uint32_t spare_observed = sun.sunrise - epoch + 7200;
    const shelly_sample spare = {.power_known = true, .energy_known = true, .total_mwh = 1000};
    elapsed = spare_observed;
    observation(1, SHELLY_OK, &spare, NULL, true, elapsed);
    observation(2, SHELLY_OK, &spare, NULL, true, elapsed);
    sample(elapsed, 700, 1000000);
    runtime[0].since = elapsed - DISPATCH_MIN_ON;
    runtime[2].since = elapsed - DISPATCH_MIN_OFF;
    solar.result = ECU_OK;
    solar.sampled = solar.sustained = true;
    solar.epoch = network_epoch();
    solar.at = solar.progress_at = elapsed;
    solar.reading.online = 1;
    solar.reading.watts = solar.supported_watts = 1500;
    dispatch_decision decisions[DISPATCH_LIMIT];
    dispatch_inputs in = inputs(elapsed, replay_clock());
    decide(&in, decisions, replay_clock());
    assert(in.solar.bridge.daylight && !sunset_history.cutoff && decisions[0].reason == DISPATCH_SOLAR_COVERED);
    assert(decisions[2].action == DISPATCH_ON && decisions[2].reason == DISPATCH_SOLAR_START);

    sample(elapsed + 5, 700, 1000972);
    observation(1, SHELLY_NETWORK, &spare, NULL, true, elapsed);
    timeline_flush_events();
    const uint32_t events_after_miss = timeline_event_cursor();
    timeline_event event;
    assert(events_after_miss && timeline_event_read(events_after_miss - 1, &event));
    assert(event.slot == 1 && event.kind == EVENT_OUTLET_LOST);
    update_solar_shed_timer(elapsed);
    update_daylight_bridge(elapsed);
    timeline_flush_events();
    assert(!bridge_active && !state.bridge_started && !paid_authority);
    assert(timeline_event_cursor() == events_after_miss);
    sample(elapsed + 5, 700, 1001944);
    assert(!state.budget.used_nano_cents && !telemetry_miner(0)->paid_mwh);
    in = inputs(elapsed, replay_clock());
    decide(&in, decisions, replay_clock());
    assert(in.solar.fresh && !in.solar.load_known && in.solar.load_retained && !in.accounting_valid && !in.outlet[1].fresh);
    assert(in.solar.bridge.unobserved_watts == DISPATCH_METER_CHANNEL_WATTS);
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_WAIT);
    assert(decisions[0].action == DISPATCH_KEEP && decisions[0].reason == DISPATCH_SOLAR_COVERED);
    assert(decisions[1].action == DISPATCH_KEEP && decisions[1].reason == DISPATCH_UNOBSERVED);
    assert(decisions[2].action == DISPATCH_KEEP && decisions[2].reason == DISPATCH_SOLAR_LOAD_UNKNOWN);
    installation_read(&view);
    assert(view.energy.solar_covered_watts == 700);

    solar.reading.watts = solar.supported_watts = 1100;
    update_solar_shed_timer(elapsed);
    update_daylight_bridge(elapsed);
    timeline_flush_events();
    assert(bridge_active && state.bridge_started == (uint64_t)replay_clock() && paid_authority == TIMELINE_PAID_GAP);
    assert(timeline_event_cursor() == events_after_miss + 1);
    assert(timeline_event_read(events_after_miss, &event) && event.kind == EVENT_DAYLIGHT_BRIDGE_STARTED);
    in = inputs(elapsed, replay_clock());
    in.used_nano_cents = 21000000000ULL;
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_BUDGET);
    state.budget.gap_mask = 1u << 1;
    in = inputs(elapsed, replay_clock());
    assert(dispatch_paid_permission(&in) == DISPATCH_BUDGET_UNKNOWN);
    state.budget.gap_mask = 0;
    solar.reading.watts = solar.supported_watts = 1500;
    update_daylight_bridge(elapsed);
    assert(!bridge_active && !state.bridge_started);

    runtime[1].pending = DISPATCH_ON;
    in = inputs(elapsed, replay_clock());
    assert(dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_EVIDENCE);
    runtime[1].pending = DISPATCH_KEEP;

    elapsed = spare_observed + SOURCE_GRACE_SECONDS;
    observation(2, SHELLY_OK, &spare, NULL, true, elapsed);
    sample(elapsed, 700, 1010000);
    assert(inputs(elapsed, replay_clock()).solar.load_retained);
    sample(elapsed + 1, 700, 1010200);
    in = inputs(elapsed, replay_clock());
    assert(!in.solar.load_retained && dispatch_paid_permission(&in) == DISPATCH_DAYLIGHT_EVIDENCE);

    observation(1, SHELLY_OK, &spare, NULL, true, elapsed);
    observation(1, SHELLY_IDENTITY, &spare, NULL, true, elapsed);
    observation(1, SHELLY_NETWORK, &spare, NULL, true, elapsed);
    assert(!inputs(elapsed, replay_clock()).solar.load_retained);
    observation(1, SHELLY_OK, &spare, NULL, true, elapsed);
    ++runtime[1].connection_epoch;
    observation(1, SHELLY_NETWORK, &spare, NULL, true, elapsed);
    assert(!inputs(elapsed, replay_clock()).solar.load_retained);
    puts("An outlet last confirmed off keeps solar coverage until its evidence expires");
}

static void checkpoint_solar_meter(void)
{
    state.solar = (solar_settings){.ip = "10.241.0.3", .port = 8899};
    state.follow.reserve = 500;
    state.settings.policy.rate_millicents_per_kwh = 13900;
    state.settings.device[0].shelly = (shelly_target){.ip = "10.241.0.1", .identity = "shellyplusplugsus-000000000001"};
    state.settings.device[1].shelly = (shelly_target){.ip = "10.241.0.2", .identity = "shellyplusplugsus-000000000002"};
    elapsed = 420;
    const shelly_sample spare = {.power_known = true, .energy_known = true, .total_mwh = 1000};
    observation(1, SHELLY_OK, &spare, NULL, true, elapsed);
    sample(elapsed, 733, 1000000);
    assert(save() == ESP_OK);
    timeline_sample row = {.solar_min = 284, .solar_max = 287, .solar_count = 30, .grid_decawatts = TIMELINE_UNKNOWN_GRID};
    row.miner[0] = (timeline_miner){.identity = installation_equipment_identity(&state.settings.device[0]),
        .maximum = 736, .observations = 60, .status = TIMELINE_OUTLET_ON};
    row.miner[1] = (timeline_miner){.identity = installation_equipment_identity(&state.settings.device[1]),
        .observations = 60, .status = TIMELINE_OUTLET_OFF};
    timeline_sample_write(replay_clock() / TIMELINE_SECONDS, &row);
}

static void reconcile_solar_meter(const char *scenario)
{
    const uint32_t resumed = 672;
    const uint64_t counter = 1051986;
    const shelly_sample spare = {.power_known = true, .energy_known = true, .total_mwh = 1000};
    assert(state.budget.anchor[0].epoch == (uint64_t)epoch + 420);
    sample(resumed, 734, counter);
    assert(state.budget.anchor[0].total_mwh == 1000000 && runtime[0].meter_wait_until);
    assert(!state.budget.used_nano_cents);
    observation(1, SHELLY_OK, &spare, NULL, true, elapsed);

    const bool missing = !strcmp(scenario, "meter-reboot-missing");
    const bool covered = !strcmp(scenario, "meter-reboot-solar");
    if (!missing) {
        solar.result = ECU_OK;
        solar.sampled = true;
        solar.epoch = network_epoch();
        solar.at = solar.progress_at = elapsed;
        solar.reading.online = 1;
        solar.reading.watts = covered ? 2715 : 1000;
    }
    if (missing) {
        sample(resumed + METER_RECOVERY_WAIT - 1, 734, counter);
        assert(state.budget.anchor[0].total_mwh == 1000000);
    }
    sample(resumed + (missing ? METER_RECOVERY_WAIT : 5), 734, counter);
    assert(state.budget.anchor[0].total_mwh == counter && !runtime[0].meter_wait_until);
    assert(telemetry_miner(0)->status & TIMELINE_CATCHUP);
    const uint64_t solar_mwh = state.solar_days[state.budget.day % DISPATCH_EVIDENCE_DAYS].energy_mwh;
    if (covered) {
        assert(solar_mwh == counter - 1000000);
        assert(!state.budget.used_nano_cents && !telemetry_miner(0)->paid_mwh);
        sample(elapsed + 5, 734, counter + 1019);
        assert(!state.budget.used_nano_cents);
    } else {
        const uint64_t observed_solar = missing ? 0 : 500 * 5 * 1000 / 3600;
        assert(solar_mwh == observed_solar);
        const uint64_t paid = counter - 1000000 - observed_solar;
        assert(state.budget.used_nano_cents == paid * 13900 && telemetry_miner(0)->paid_mwh == paid);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3 || !preview_storage_open(argv[1])) return 1;
    installation_start();
    if (!strcmp(argv[2], "meter-checkpoint")) {
        checkpoint_solar_meter();
        return 0;
    }
    if (!strncmp(argv[2], "meter-reboot-", 13)) {
        reconcile_solar_meter(argv[2]);
        return 0;
    }
    if (!strcmp(argv[2], "daylight-dropout")) {
        daylight_outlet_dropout();
        return 0;
    }
    if (!strcmp(argv[2], "sunset") || !strcmp(argv[2], "sunset-restart")) {
        sunset_replay(!strcmp(argv[2], "sunset-restart"));
        return 0;
    }
    if (!strcmp(argv[2], "arm-restored-command")) {
        assert(state.settings.device[0].shelly.ip[0]);
        state.enabled = true;
        state.test_armed = 1;
        state.hold_mask = state.hold_on = state.attempt_counts = 1;
        state.evaluation.active = true;
        return save() != ESP_OK;
    }
    assert(!strcmp(argv[2], "replay") || !strcmp(argv[2], "restart"));
    replay(!strcmp(argv[2], "restart"));
}
