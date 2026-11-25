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
    if (!grid.importing) grid.covered_at = when;
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

int main(int argc, char **argv)
{
    if (argc != 3 || !preview_storage_open(argv[1])) return 1;
    installation_start();
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
