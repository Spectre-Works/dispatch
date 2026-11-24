// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "installation.h"
#include "discovery.h"
#include "network.h"
#include "network_update.h"
#include "profile_store.h"
#include "timeline.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

extern const char web_index[] asm("_binary_index_html_start");
extern const char web_gzip[] asm("_binary_dispatch_index_html_gz_start");
extern const char web_gzip_end[] asm("_binary_dispatch_index_html_gz_end");
enum { FORM_BYTES = 2304, IMPORT_FORM_BYTES = 2304, SINGLE_FORM_BYTES = 896 };
static uint32_t web_stack;
static size_t lowest_largest = SIZE_MAX;
static installation_view snapshot;
static discovery_view discovered;
static char form[FORM_BYTES + 1];
static char output[768];
static size_t output_used;
static esp_err_t output_error;

static void headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Connection", "close");
}

static esp_err_t reply(httpd_req_t *request, const char *status, const char *message)
{
    headers(request);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, message);
}

static bool authorize(httpd_req_t *request)
{
    char key[OWNER_KEY_LENGTH + 1];
    bool valid = httpd_req_get_hdr_value_str(request, "X-Dispatch-Key", key, sizeof(key)) == ESP_OK &&
        network_authorized(key);
    explicit_bzero(key, sizeof(key));
    return valid;
}

static bool accepts_gzip(httpd_req_t *request)
{
    char header[128], *rest;
    if (httpd_req_get_hdr_value_str(request, "Accept-Encoding", header, sizeof(header)) != ESP_OK) {
        return false;
    }
    for (char *coding = strtok_r(header, ",", &rest); coding; coding = strtok_r(NULL, ",", &rest)) {
        while (isspace((unsigned char)*coding)) ++coding;
        if (strncasecmp(coding, "gzip", 4)) continue;
        coding += 4;
        while (isspace((unsigned char)*coding)) ++coding;
        if (!*coding) return true;
        if (*coding == ';') {
            ++coding;
            while (isspace((unsigned char)*coding)) ++coding;
            if (!strncasecmp(coding, "q=", 2) && strtod(coding + 2, NULL) > 0) return true;
        }
    }
    return false;
}

static esp_err_t index_page(httpd_req_t *request)
{
    headers(request);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
        "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
    httpd_resp_set_hdr(request, "Vary", "Accept-Encoding");
    if (accepts_gzip(request)) {
        httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
        return httpd_resp_send(request, web_gzip, web_gzip_end - web_gzip);
    }
    return httpd_resp_sendstr(request, web_index);
}

static void add(httpd_req_t *request, const char *format, ...)
{
    char item[480];
    if (output_error != ESP_OK) {
        return;
    }
    va_list args;
    va_start(args, format);
    int size = vsnprintf(item, sizeof(item), format, args);
    va_end(args);
    if (size < 0 || (size_t)size >= sizeof(item)) {
        output_error = ESP_ERR_INVALID_SIZE;
        return;
    }
    if (output_used + size > sizeof(output)) {
        output_error = httpd_resp_send_chunk(request, output, output_used);
        output_used = 0;
    }
    memcpy(output + output_used, item, size);
    output_used += size;
}

static const char *truth(bool value)
{
    return value ? "true" : "false";
}

static void json_begin(httpd_req_t *request)
{
    headers(request);
    httpd_resp_set_type(request, "application/json");
    output_used = 0;
    output_error = ESP_OK;
}

static esp_err_t json_end(httpd_req_t *request)
{
    if (output_error == ESP_OK && output_used) {
        output_error = httpd_resp_send_chunk(request, output, output_used);
    }
    return output_error == ESP_OK ? httpd_resp_send_chunk(request, NULL, 0) : output_error;
}

static void nullable(httpd_req_t *request, const char *key, bool known, double value)
{
    if (known) {
        add(request, ",\"%s\":%.3f", key, value);
    } else {
        add(request, ",\"%s\":null", key);
    }
}

static void equipment_name_status(httpd_req_t *request, const char *name)
{
    char escaped[2 * EQUIPMENT_NAME_BYTES];
    char *out = escaped;
    for (size_t i = 0; i < EQUIPMENT_NAME_BYTES - 1 && name[i]; ++i) {
        if (name[i] == '"' || name[i] == '\\') *out++ = '\\';
        *out++ = name[i];
    }
    *out = 0;
    add(request, ",\"name\":\"%s\"", escaped);
}

static void preset_totals_status(httpd_req_t *request, const dispatch_preset_totals *totals);

static void device_status(httpd_req_t *request, unsigned i)
{
    const device_view *device = &snapshot.device[i];
    const association *assoc = &snapshot.settings.device[i];
    static const char *const action[] = {"Keep state", "Switch off", "Switch on"};
    static const char *const outlet_errors[] = {
        [SHELLY_OK] = "Fresh", [SHELLY_NETWORK] = "Unreachable",
        [SHELLY_AUTH] = "Authentication required or rejected",
        [SHELLY_PROTOCOL] = "Invalid or unsupported reply",
        [SHELLY_IDENTITY] = "Device ID does not match",
    };
    const char *reason = dispatch_explanation(device->decision.reason);
    if (device->decision.reason == DISPATCH_CLOCK && snapshot.time_synchronized) {
        reason = snapshot.local_day ? "Aligning the electricity budget date" : "Check the installation time zone";
    }
    add(request, "%s{\"ip\":\"%s\",\"identity\":\"%s\",\"minerIp\":\"%s\","
        "\"outlet\":%s,\"outletStatus\":\"%s\",\"miner\":%s,\"summary\":%s,\"pending\":%s,",
        i ? "," : "", assoc->shelly.ip, assoc->shelly.identity, assoc->miner_ip,
        device->observed ? truth(device->outlet.on) : "null",
        device->observed || device->outlet_result != SHELLY_OK ? outlet_errors[device->outlet_result] : "Awaiting fresh sample",
        device->miner_polled ? truth(device->miner.responding) : "null",
        truth(device->miner.summary_available), truth(device->command_pending));
    add(request, "\"action\":\"%s\",\"reason\":\"%s\"", action[device->decision.action], reason);
    add(request, ",\"outletKind\":%u,\"timedTest\":%s",
        (unsigned)outlet_identity(assoc->shelly.identity), truth(outlet_timed_test(assoc->shelly.identity) && device->outlet.power_known && device->outlet.energy_known));
    add(request, ",\"timelineId\":%u", installation_equipment_identity(assoc));
    if (outlet_identity(assoc->shelly.identity) == OUTLET_HA) {
        const home_meter_mapping *meter = &snapshot.home_meter[i];
        add(request, ",\"homeMeter\":{\"power\":\"%s\",\"energy\":\"%s\"}", meter->power, meter->energy);
    }
    const power_test_view *test = &device->power_test;
    add(request, ",\"powerTest\":{\"phase\":%u,\"seconds\":%u,\"peak\":%u,\"stableSeconds\":%u,"
        "\"measuredPeak\":%u,\"measuredRunning\":%u,\"needsEstimate\":%s",
        (unsigned)test->phase, test->seconds, test->peak, test->stable_seconds,
        test->measured.peak, test->measured.running, truth(device->decision.reason == DISPATCH_SURPLUS_POWER));
    add(request, ",\"blocked\":\"%s\",\"detail\":\"%s\"}", test->blocked ? test->blocked : "",
        test->detail ? test->detail : "");
    equipment_name_status(request, device->name);
    add(request, ",\"workMissing\":%s,\"recoveryExhausted\":%s",
        truth(device->work_missing), truth(device->recovery_exhausted));
    add(request, ",\"powerMode\":{\"status\":%u,\"mode\":%u,\"saved\":%u,\"alternate\":%u,\"idle\":%s,\"pending\":%s,\"current\":%s,\"blockedBy\":\"%s\",\"waitSeconds\":%u}",
        (unsigned)device->power_mode.status, (unsigned)device->power_mode.mode, (unsigned)device->power_mode.saved,
        (unsigned)device->power_mode.alternate, truth(device->power_mode.idle_supported),
        truth(device->power_pending), truth(device->power_current), device->power_block ? device->power_block : "",
        device->running_seconds < DISPATCH_MIN_ON ? (unsigned)(DISPATCH_MIN_ON - device->running_seconds) : 0);
    add(request, ",\"restart\":{\"supported\":%s,\"ready\":%s,\"command\":%u,\"result\":%u}",
        truth(device->power_mode.restart_supported), truth(device->restart_ready),
        (unsigned)device->restart_command, (unsigned)device->restart_result);
    add(request, ",\"minerModel\":\"%s\",\"minerFirmware\":\"%s\",\"experimentalPresets\":%s",
        device->power_mode.model ? device->power_mode.model : device->power_mode.advertised_model,
        device->power_mode.firmware,
        truth(device->power_mode.experimental));
    add(request, ",\"presetControl\":{\"authorized\":%s,\"eligible\":%s,\"authFailed\":%s,"
        "\"evaluating\":%s,\"restoring\":%s,\"target\":%u,\"request\":%u}",
        truth(device->preset_authorized), truth(device->preset_evaluable), truth(device->preset_auth_failed),
        truth(device->preset_evaluating), truth(device->preset_restoring),
        (unsigned)device->preset_target, (unsigned)device->preset_request);
    add(request, ",\"outletCode\":%u,\"runningSeconds\":%u,\"solarRestartSeconds\":%u,\"commandHeld\":%s,\"stalled\":%s,"
        "\"firstRun\":{\"mapping\":%s,\"power\":%s,\"work\":%s}",
        (unsigned)device->outlet_result, (unsigned)device->running_seconds, (unsigned)device->solar_restart_seconds,
        truth(device->decision.reason == DISPATCH_COMMAND_HOLD), truth(device->stalled),
        truth(device->first_run & RUN_MAPPING), truth(device->first_run & RUN_POWER),
        truth(device->first_run & RUN_WORK));
    nullable(request, "sampleAge", device->observed, snapshot.now - device->outlet_time);
    nullable(request, "watts", device->observed && device->outlet.power_known, device->outlet.watts);
    nullable(request, "wh", device->observed && device->outlet.energy_known, device->outlet.total_mwh / 1000.0);
    nullable(request, "mhs", device->miner.activity_known, device->miner.mhs);
    add(request, ",\"rateWindow\":%u", (unsigned)device->miner.rate_window_seconds);
    nullable(request, "accepted", device->miner.accepted_known, (double)device->miner.accepted);
    nullable(request, "minerAge", device->miner_polled, snapshot.now - device->miner_time);
    const dispatch_profile *profile = &device->profile;
    nullable(request, "runningWatts", profile->power_samples > 0, profile->mean_watts);
    nullable(request, "learnedStartupWatts", device->learned_startup_watts > 0, device->learned_startup_watts);
    nullable(request, "startup", profile->startups > 0, profile->startup_seconds);
    nullable(request, "learnedMhs", profile->hash_samples > 0, profile->mean_mhs);
    nullable(request, "minMhs", profile->hash_samples > 0, profile->min_mhs);
    nullable(request, "maxMhs", profile->hash_samples > 0, profile->max_mhs);
    nullable(request, "minWatts", profile->power_samples > 0, profile->min_watts);
    nullable(request, "maxWatts", profile->power_samples > 0, profile->max_watts);
    if (device->efficiency.samples) {
        add(request, ",\"efficiency\":%.6g", device->efficiency.mean_j_per_gh);
    } else {
        add(request, ",\"efficiency\":null");
    }
    add(request, ",\"efficiencySamples\":%u", device->efficiency.samples);
    if (device->power_mode.alternate || device->measured_alternate) {
        const dispatch_preset_evidence *normal = &device->presets[PRESET_DEFAULT];
        const dispatch_preset_evidence *reduced = &device->presets[PRESET_REDUCED];
        dispatch_preset_totals combined = normal->totals;
        dispatch_preset_merge(&combined, &reduced->totals);
        add(request, ",\"presetEfficiency\":{\"alternate\":%u,\"comparison\":%u,\"defaultWindows\":%u,\"reducedWindows\":%u,\"default\":",
            (unsigned)(device->measured_alternate ? device->measured_alternate : device->power_mode.alternate),
            (unsigned)dispatch_preset_compare(normal, reduced), dispatch_preset_window_count(normal),
            dispatch_preset_window_count(reduced));
        preset_totals_status(request, &normal->totals);
        add(request, ",\"reduced\":");
        preset_totals_status(request, &reduced->totals);
        add(request, ",\"combined\":");
        preset_totals_status(request, &combined);
        add(request, "}");
    }
    add(request, ",\"powerSamples\":%u,\"hashSamples\":%u,\"startupSamples\":%u",
        profile->power_samples, profile->hash_samples, profile->startups);
    add(request, ",\"priority\":%u,\"recoveryEnabled\":%s,\"overrideMode\":%u,\"overrideSeconds\":%u,"
        "\"recoverySeconds\":%u,\"destination\":%u,\"recoveryStatus\":\"%s\"",
        device->coordination.priority, truth(device->coordination.recovery),
        device->override_seconds ? device->coordination.mode : OVERRIDE_NONE,
        (unsigned)device->override_seconds, (unsigned)device->recovery_seconds,
        (unsigned)device->destination, device->recovery_status);
    add(request, "}");
}

static void preset_totals_status(httpd_req_t *request, const dispatch_preset_totals *totals)
{
    if (!totals->seconds) {
        add(request, "null");
        return;
    }
    add(request, "{\"seconds\":%u,\"watts\":%.2f,\"ths\":%.4g,\"jth\":%.4g}",
        (unsigned)totals->seconds, totals->joules / totals->seconds, totals->terahashes / totals->seconds,
        dispatch_preset_efficiency(totals));
}

static void daily_status(httpd_req_t *request, unsigned i)
{
    const dispatch_daily_evidence *day = &snapshot.days[i];
    const solar_daily_evidence *solar_day = &snapshot.solar_days[i];
    add(request, "{\"day\":%u,\"kwh\":%.6f,\"cents\":%.6f,\"poweredSeconds\":%u,"
        "\"interruptions\":%u,\"metered\":%s,\"gapAccepted\":%s,"
        "\"solarKwh\":%.6f,\"solarSavingsCents\":%.6f,\"presetEfficiency\":[",
        (unsigned)day->day, day->energy_mwh / 1000000.0, day->cost_nano_cents / 1000000000.0,
        (unsigned)day->powered_seconds, (unsigned)day->interruptions, truth(day->metered), truth(day->gap_accepted),
        solar_day->energy_mwh / 1000000.0, solar_day->savings_nano_cents / 1000000000.0);
    for (unsigned slot = 0; slot < DISPATCH_LIMIT; ++slot) {
        if (slot) add(request, ",");
        const preset_daily_evidence *evidence = &snapshot.preset_days[i];
        if (evidence->day == day->day) preset_totals_status(request, &evidence->device[slot]);
        else add(request, "null");
    }
    add(request, "]}");
}

static void home_text(httpd_req_t *request, const char *text)
{
    char escaped[2 * HA_OPTION_BYTES], *out = escaped;
    for (; *text; ++text) {
        if (*text == '"' || *text == '\\') *out++ = '\\';
        *out++ = *text;
    }
    *out = 0;
    add(request, "\"%s\"", escaped);
}

static void home_status(httpd_req_t *request)
{
    const home_view *home = &snapshot.home;
    add(request, "\"home\":{\"ip\":\"%s\",\"port\":%u,\"battery\":\"%s\",\"temperature\":\"%s\","
        "\"actuator\":\"%s\",\"known\":%s,\"command\":%u,\"result\":%u",
        home->ip, home->port, home->battery, home->temperature, home->actuator,
        truth(home->actuator_known), (unsigned)home->command, (unsigned)home->result);
    nullable(request, "batteryPercent", home->battery_known, home->battery_percent);
    nullable(request, "temperatureC", home->temperature_known, home->temperature_c);
    if (home->actuator_known) {
        const ha_control_state *control = &home->control;
        add(request, ",\"value\":%.17g,\"min\":%.17g,\"max\":%.17g,\"step\":%.17g,\"text\":",
            control->value, control->min, control->max, control->step);
        home_text(request, control->text);
        add(request, ",\"options\":[");
        for (unsigned i = 0; i < control->options; ++i) {
            if (i) add(request, ",");
            home_text(request, control->option[i]);
        }
        add(request, "]");
    }
    add(request, "},");
}

static void energy_status(httpd_req_t *request)
{
    const energy_view *energy = &snapshot.energy;
    const solar_settings *solar = &energy->solar;
    const grid_settings *grid = &energy->grid;
    const ecu_reading *production = &energy->production;
    add(request, "\"energy\":{\"solar\":{\"ip\":\"%s\",\"port\":%u,\"expected\":\"%s\",\"source\":\"%s\","
        "\"nightStart\":%u,\"nightEnd\":%u,\"night\":%s,\"status\":%u,\"fresh\":%s,"
        "\"id\":\"%s\",\"firmware\":\"%s\"",
        solar->ip, solar->port, solar->id, solar->ip[0] && !solar->port ? "bridge" : "ecu", solar->night_start, solar->night_end, truth(energy->solar_night),
        (unsigned)energy->solar_result, truth(energy->solar_fresh), production->id, production->firmware);
    nullable(request, "age", energy->solar_sampled, energy->solar_age);
    nullable(request, "watts", energy->solar_sampled, production->watts);
    nullable(request, "today", energy->solar_sampled, production->today_centi_kwh / 100.0);
    nullable(request, "lifetime", energy->solar_sampled, production->lifetime_deci_kwh / 10.0);
    nullable(request, "inverters", energy->solar_sampled, production->inverters);
    nullable(request, "online", energy->solar_sampled, production->online);
    add(request, "},\"grid\":{\"ip\":\"%s\",\"identity\":\"%s\",\"verified\":%s,"
        "\"reserve\":%u,\"status\":%u,\"attempted\":%s,\"fresh\":%s",
        grid->target.ip, grid->target.identity, truth(grid->direction_verified), grid->reserve,
        (unsigned)energy->grid_result, truth(energy->grid_attempted), truth(energy->grid_fresh));
    const native_grid_kind kind = native_grid_type(grid->target.identity);
    if (kind) {
        add(request, ",\"source\":\"%s\",\"serial\":\"%s\",\"port\":%u,\"unit\":%u,\"address\":%u,"
            "\"model\":%u,\"meterIndex\":%u,\"function\":%u,\"exponent\":%d,\"reverse\":%s",
            kind == NATIVE_GRID_SUNSPEC ? "sunspec" : "solarman-v5", grid->target.identity + 4,
            grid->native.port, grid->native.unit, grid->native.address, grid->native.model,
            grid->native.meter_index, grid->native.function, grid->native.exponent, truth(grid->reverse));
    }
    nullable(request, "age", energy->grid_sampled, energy->grid_age);
    nullable(request, "watts", energy->grid_sampled, energy->grid_watts);
    add(request, ",\"gxCommand\":%u,\"gxResult\":%u,\"gxRequested\":%ld",
        (unsigned)energy->gx_command, (unsigned)energy->gx_result, (long)energy->gx_requested);
    nullable(request, "gxTarget", energy->gx_target_known, energy->gx_target);
    nullable(request, "froniusSolarWatts", energy->fronius_solar_known, energy->fronius_solar_watts);
    const follow_settings *follow = &energy->follow;
    const dispatch_generation *decision = &energy->generation;
    add(request, "},\"follow\":{\"reserve\":%u,\"margin\":%u,\"fresh\":%s,"
        "\"sustained\":%s,\"loadKnown\":%s,\"qualifiedWatts\":%lu,"
        "\"runningWatts\":%lu,\"committedWatts\":%lu,\"headroom\":%lld,"
        "\"start\":[%u,%u,%u,%u],\"expected\":[%u,%u,%u,%u]",
        follow->reserve, follow->shed_margin, truth(decision->fresh),
        truth(decision->sustained), truth(decision->load_known),
        (unsigned long)decision->supported_production, (unsigned long)decision->running_watts,
        (unsigned long)decision->committed_watts, (long long)dispatch_solar_headroom(decision),
        follow->start_watts[0], follow->start_watts[1], follow->start_watts[2], follow->start_watts[3],
        decision->start_watts[0], decision->start_watts[1], decision->start_watts[2], decision->start_watts[3]);
    add(request, ",\"restartMinutes\":%u,\"shedMinutes\":%u,\"low\":%s,\"shedSeconds\":%u,\"shedNeedsReading\":%s",
        follow->restart_seconds / 60, follow->shed_delay_seconds / 60, truth(decision->low), decision->shed_wait_seconds,
        truth(decision->shed_needs_reading));
    const dispatch_reason paid = energy->paid_permission;
    add(request, ",\"paidOpen\":%s,\"paidReason\":\"%s\"",
        truth(dispatch_paid_covering(paid)), dispatch_explanation(paid));
    const bool power_known = decision->load_known && (decision->fresh || !decision->configured);
    nullable(request, "solarCoveredWatts", power_known, energy->solar_covered_watts);
    nullable(request, "paidWatts", power_known, decision->running_watts - energy->solar_covered_watts);
    const daylight_settings *daylight = &energy->daylight;
    add(request, ",\"daylight\":{\"enabled\":%s,\"active\":%s,\"configured\":%s,\"kind\":%u,\"minutes\":%u,"
        "\"inside\":%s,\"seconds\":%lu,\"latitude\":%.4f,\"longitude\":%.4f",
        truth(daylight->enabled), truth(paid == DISPATCH_DAYLIGHT_BRIDGE), truth(daylight->minutes), (unsigned)energy->daylight_kind,
        daylight->minutes ? daylight->minutes : BRIDGE_DEFAULT_MINUTES, truth(decision->bridge.daylight),
        (unsigned long)decision->bridge.seconds, daylight->latitude_e4 / 10000.0, daylight->longitude_e4 / 10000.0);
    nullable(request, "sunrise", energy->daylight_kind == DAYLIGHT_RISE_SET, energy->sunrise_minute);
    nullable(request, "sunset", energy->daylight_kind == DAYLIGHT_RISE_SET, energy->sunset_minute);
    add(request, "}");
    const dispatch_gap_forecast *forecast = &energy->gap_budget;
    const dispatch_gap_forecast *after = &energy->gap_budget_after;
    if (forecast->known) {
        add(request, ",\"gapBudget\":{\"paidWatts\":%lu,\"runwaySeconds\":%lu,\"sunsetSeconds\":%lu",
            (unsigned long)forecast->paid_watts, (unsigned long)forecast->runway_seconds,
            (unsigned long)forecast->seconds_to_sunset);
        nullable(request, "afterWatts", after->known, after->paid_watts);
        nullable(request, "afterSeconds", after->known, after->runway_seconds);
        add(request, "}");
    }
    add(request, "}},");
}

static esp_err_t reports_page(httpd_req_t *request)
{
    if (!authorize(request) || network_setup_active()) {
        return reply(request, "401 Unauthorized", "Enter the owner key.");
    }
    installation_read(&snapshot);
    json_begin(request);
    add(request, "[");
    uint32_t newest = snapshot.local_day;
    if (!newest) {
        for (unsigned i = 0; i < DISPATCH_EVIDENCE_DAYS; ++i) {
            if (snapshot.days[i].day > newest) newest = snapshot.days[i].day;
        }
    }
    bool first = true;
    for (unsigned i = 0; i < DISPATCH_EVIDENCE_DAYS; ++i) {
        const uint32_t day = snapshot.days[i].day;
        if (day && (day >= newest || newest - day < DISPATCH_EVIDENCE_DAYS)) {
            if (!first) add(request, ",");
            daily_status(request, i);
            first = false;
        }
    }
    add(request, "]");
    return json_end(request);
}

static bool timeline_parameter(const char *query, const char *name, uint32_t *value)
{
    char field[16];
    const esp_err_t result = httpd_query_key_value(query, name, field, sizeof(field));
    if (result == ESP_ERR_NOT_FOUND) return true;
    if (result != ESP_OK || !field[0]) return false;
    uint64_t number = 0;
    for (const char *c = field; *c; ++c) {
        if (*c < '0' || *c > '9') return false;
        number = number * 10 + (*c - '0');
        if (number > UINT32_MAX) return false;
    }
    *value = number;
    return true;
}

static void timeline_write_samples(httpd_req_t *request, unsigned tier, uint32_t first, uint32_t end)
{
    _Static_assert((unsigned)DISPATCH_START_WINDOW_SHORT <= TIMELINE_REASON_MASK, "Activity reason mask capacity");
    uint64_t reasons = 0;
    bool emitted = false;
    add(request, ",\"samples\":[");
    for (uint32_t bucket = first; bucket < end && output_error == ESP_OK; ++bucket) {
        timeline_sample sample;
        if (!timeline_sample_read(tier, bucket, &sample)) continue;
        add(request, "%s{\"at\":%lu", emitted ? "," : "", (unsigned long)bucket * timeline_period(tier));
        emitted = true;
        nullable(request, "solar", sample.solar_decawatts != TIMELINE_UNKNOWN_WATTS, sample.solar_decawatts * 10);
        nullable(request, "grid", sample.grid_decawatts != TIMELINE_UNKNOWN_GRID, sample.grid_decawatts * 10);
        nullable(request, "spent", sample.flags & TIMELINE_BUDGET, sample.spent_millicents / 1000.0);
        nullable(request, "limit", sample.flags & TIMELINE_BUDGET && sample.allowance_millicents, sample.allowance_millicents / 1000.0);
        add(request, ",\"flags\":%u,\"productive\":%u,\"replacedMwh\":%lu,\"replacedMillicents\":%lu",
            sample.flags, sample.productive, (unsigned long)sample.replaced_mwh, (unsigned long)sample.replaced_millicents);
        nullable(request, "solarMin", sample.solar_count, sample.solar_min * 10);
        nullable(request, "solarMax", sample.solar_count, sample.solar_max * 10);
        nullable(request, "gridMin", sample.grid_count, sample.grid_min * 10);
        nullable(request, "gridMax", sample.grid_count, sample.grid_max * 10);
        add(request, ",\"miners\":[");
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            const timeline_miner *m = &sample.miner[i];
            if (i) add(request, ",");
            if (!m->identity) {
                add(request, "null");
                continue;
            }
            const unsigned reason = m->reason & TIMELINE_REASON_MASK;
            add(request, "[%u,%d,%lu,%.1f,%u,%u,%u,%d,%d,%u]", m->identity,
                m->watts == TIMELINE_UNKNOWN_WATTS ? -1 : m->watts,
                (unsigned long)m->paid_mwh, m->cost_millicents / 10.0, reason, m->status, (unsigned)m->reason >> 6,
                m->observations ? m->minimum : -1, m->observations ? m->maximum : -1, m->observations);
            if (reason <= DISPATCH_START_WINDOW_SHORT) reasons |= 1ULL << reason;
        }
        add(request, "]}");
    }
    add(request, "],\"reasons\":{");
    emitted = false;
    for (unsigned i = 0; i <= DISPATCH_START_WINDOW_SHORT; ++i) {
        if (!(reasons & (1ULL << i))) continue;
        const char *reason = i == TIMELINE_GRID_HOLD ? "The whole-site grid policy kept this outlet on" :
            i == TIMELINE_GENERATION_HOLD ? "The solar generation policy kept this outlet on" : dispatch_explanation(i);
        add(request, "%s\"%u\":\"%s\"", emitted ? "," : "", i, reason);
        emitted = true;
    }
    add(request, "}");
}

static void timeline_write_events(httpd_req_t *request, uint32_t from, uint32_t before)
{
    enum { SCAN_LIMIT = 128, EVENT_LIMIT = 48 };
    const unsigned capacity = timeline_event_capacity();
    const uint32_t cursor = timeline_event_cursor();
    const uint32_t earliest = cursor > capacity ? cursor - capacity : 0;
    uint32_t position = before < cursor ? before : cursor;
    unsigned emitted = 0;
    add(request, ",\"events\":[");
    for (unsigned scanned = 0; position > earliest && scanned < SCAN_LIMIT &&
        emitted < EVENT_LIMIT && output_error == ESP_OK; ++scanned) {
        timeline_event event;
        if (!timeline_event_read(--position, &event)) continue;
        if (event.epoch < from) { position = earliest; break; }
        const char *lane = "relay", *edge = "note";
        switch (event.kind) {
            case EVENT_DAYLIGHT_BRIDGE_STARTED: lane = "bridge"; edge = "start"; break;
            case EVENT_DAYLIGHT_BRIDGE_REARMED: case EVENT_DAYLIGHT_BRIDGE_ENDED: lane = "bridge"; edge = "end"; break;
            case EVENT_DEFAULT_MODE: case EVENT_LOW_MODE: case EVENT_BALANCE_MODE: case EVENT_IDLE_MODE:
                lane = "preset"; edge = "change"; break;
            case EVENT_POWER_MODE_UNCONFIRMED: case EVENT_PRESET_SOLAR_LOWER: case EVENT_PRESET_SOLAR_RESTORE:
            case EVENT_PRESET_EVAL_APPLIED: case EVENT_PRESET_EFFICIENT: case EVENT_PRESET_BRIDGE_LOWER: lane = "preset"; break;
            case EVENT_OUTLET_ON: edge = "on"; break;
            case EVENT_OUTLET_OFF: edge = "off"; break;
            case EVENT_POWER_TEST_STARTED: edge = "on"; break;
            case EVENT_POWER_TEST_COMPLETE: case EVENT_POWER_TEST_CANCELED: case EVENT_POWER_TEST_FAILED: edge = "off"; break;
            case EVENT_ACCOUNTING_CLEARED: lane = "account"; break;
            default: break;
        }
        const char *label = installation_event(event.kind);
        if (!label) continue;
        add(request, "%s{\"at\":%lu,\"sequence\":%lu,\"slot\":%u,\"id\":%u,\"lane\":\"%s\",\"edge\":\"%s\",\"label\":\"%s\"",
            emitted++ ? "," : "", (unsigned long)event.epoch, (unsigned long)event.sequence, event.slot, event.identity, lane, edge, label);
        if (event.reason <= DISPATCH_DAYLIGHT_EVIDENCE ||
            (event.reason >= DISPATCH_POWER_TEST && event.reason <= DISPATCH_START_WINDOW_SHORT))
            add(request, ",\"reason\":\"%s\"", dispatch_explanation(event.reason));
        add(request, "}");
    }
    add(request, "],\"before\":%lu", (unsigned long)(position > earliest ? position : 0));
}

static esp_err_t timeline_page(httpd_req_t *request)
{
    if (!authorize(request) || network_setup_active()) return reply(request, "401 Unauthorized", "Enter the owner key.");
    const uint32_t now = network_clock();
    uint32_t tier = 0, end = UINT32_MAX, count = TIMELINE_PAGE, event_from = 0, before = UINT32_MAX;
    char query[112] = "";
    if (httpd_req_get_url_query_len(request) &&
        (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
         !timeline_parameter(query, "tier", &tier) || tier >= TIMELINE_TIERS ||
         !timeline_parameter(query, "end", &end) || !timeline_parameter(query, "count", &count) ||
         !timeline_parameter(query, "events", &event_from) || !timeline_parameter(query, "before", &before))) {
        return reply(request, "400 Bad Request", "Invalid activity range.");
    }
    const unsigned period = timeline_period(tier), retained = timeline_retention(tier);
    const uint32_t latest = now / period;
    const uint32_t oldest = latest > retained ? latest - retained : 0;
    if (end > latest) end = latest;
    if (count > TIMELINE_PAGE) count = TIMELINE_PAGE;
    const uint32_t first = end > count && end - count > oldest ? end - count : oldest;
    json_begin(request);
    add(request, "{\"available\":%s,\"now\":%lu,\"period\":%u,\"retained\":%u",
        truth(timeline_available()), (unsigned long)now, period, retained);
    timeline_write_samples(request, tier, first, end);
    if (event_from) timeline_write_events(request, event_from, before);
    add(request, "}");
    return json_end(request);
}

static void rate_windows_status(httpd_req_t *request, const dispatch_tariff *tariff)
{
    add(request, "[");
    for (unsigned i = 0; i < DISPATCH_RATE_WINDOWS; ++i) {
        const dispatch_rate_window *window = &tariff->window[i];
        add(request, "%s{\"enabled\":%s,\"start\":%u,\"end\":%u,\"rate\":%.3f}",
            i ? "," : "", truth(window->enabled), window->start_minute, window->end_minute,
            window->rate_millicents_per_kwh / 1000.0);
    }
    add(request, "]");
}

static esp_err_t status_page(httpd_req_t *request)
{
    if (!authorize(request)) {
        return reply(request, "401 Unauthorized", "Enter the setup or owner key.");
    }
    json_begin(request);
    if (network_setup_active()) {
        return httpd_resp_sendstr(request, "{\"setup\":true}");
    }
    installation_read(&snapshot);
    web_stack = uxTaskGetStackHighWaterMark(NULL);
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t largest = heap_caps_get_largest_free_block(caps);
    if (largest < lowest_largest) {
        lowest_largest = largest;
    }
    add(request, "{\"setup\":false,\"enabled\":%s,\"planReady\":%s,\"clock\":%s,\"paused\":%s,"
        "\"pauseSeconds\":%u,\"complete\":%s,\"storageError\":%s,\"day\":%u,"
        "\"usedCents\":%.6f,\"start\":%u,\"end\":%u,\"rate\":%.3f,\"allowance\":%.3f,"
        "\"offset\":%d,\"timezone\":\"%s\",",
        truth(snapshot.enabled), truth(snapshot.plan_ready), truth(snapshot.clock_valid), truth(snapshot.paused),
        (unsigned)snapshot.pause_seconds, truth(snapshot.accounting_complete), truth(snapshot.storage_error),
        (unsigned)snapshot.day, snapshot.used_nano_cents / 1000000000.0,
        snapshot.settings.policy.start_minute, snapshot.settings.policy.end_minute,
        snapshot.settings.policy.rate_millicents_per_kwh / 1000.0,
        snapshot.settings.policy.daily_allowance_millicents / 1000.0,
        snapshot.settings.utc_offset_minutes, snapshot.timezone);
    add(request, "\"quietEnabled\":%s,\"quietActive\":%s,\"quietStart\":%u,\"quietEnd\":%u,",
        truth(snapshot.quiet.enabled), truth(snapshot.quiet_active),
        snapshot.quiet.start_minute, snapshot.quiet.end_minute);
    add(request, "\"presetCredentialsPersistent\":%s,", truth(snapshot.preset_credentials_persistent));
    add(request, "\"operating\":{\"paidEnabled\":%s,\"paidStart\":%u,\"paidEnd\":%u,"
        "\"weekendPaidStart\":%u,\"weekendPaidEnd\":%u,\"gridSource\":%s,\"preset\":[%u,%u,%u,%u]},",
        truth(snapshot.operating.paid_enabled), snapshot.operating.paid_start,
        snapshot.operating.paid_end, snapshot.operating.weekend_paid_start, snapshot.operating.weekend_paid_end,
        truth(snapshot.energy.grid_source),
        snapshot.preset_choice[0], snapshot.preset_choice[1], snapshot.preset_choice[2], snapshot.preset_choice[3]);
    add(request, "\"pauseIndefinite\":%s,\"next\":", truth(snapshot.pause_indefinite));
    if (snapshot.next.kind) {
        add(request, "{\"label\":\"%s\",\"day\":%u,\"minute\":%u,\"seconds\":%u,\"slot\":%u,\"restartLockout\":%s},",
            installation_next_label(snapshot.next.kind), (unsigned)snapshot.next.day, (unsigned)snapshot.next.minute,
            (unsigned)snapshot.next.seconds, (unsigned)snapshot.next.slot,
            truth(snapshot.next.kind == NEXT_SOLAR_RESTART));
    } else {
        add(request, "null,");
    }
    add(request, "\"rateWindows\":");
    rate_windows_status(request, &snapshot.tariff);
    const dispatch_weekend_plan *weekend = &snapshot.weekend;
    add(request, ",\"activeWeekend\":%s,\"weekend\":{\"enabled\":%s,\"start\":%u,\"end\":%u,"
        "\"quiet\":{\"enabled\":%s,\"start\":%u,\"end\":%u},\"rateWindows\":",
        truth(snapshot.active_weekend), truth(weekend->enabled), weekend->start_minute, weekend->end_minute,
        truth(weekend->quiet.enabled), weekend->quiet.start_minute, weekend->quiet.end_minute);
    rate_windows_status(request, &weekend->tariff);
    add(request, "},\"activeRateWindow\":%u,\"highestRateUsed\":%s",
        snapshot.active_rate_window, truth(snapshot.highest_rate_used));
    nullable(request, "activeRate", snapshot.rate_known, snapshot.active_rate / 1000.0);
    add(request, ",\"notifications\":{\"ip\":\"%s\",\"port\":%u,\"sent\":%u,\"failed\":%u,"
        "\"dropped\":%u,\"pending\":%u,\"test\":%u},",
        snapshot.notifications.ip, snapshot.notifications.port, (unsigned)snapshot.notifications.sent,
        (unsigned)snapshot.notifications.failed, (unsigned)snapshot.notifications.dropped,
        snapshot.notifications.pending, (unsigned)snapshot.notifications.test);
    network_update_view update;
    network_update_read(&update);
    add(request, "\"networkUpdate\":{\"supported\":%s,\"enabled\":%s,\"unread\":%s,"
        "\"offered\":\"%s\",\"phase\":%u,\"result\":%u,\"received\":%lu,\"total\":%lu",
        truth(update.supported), truth(update.enabled), truth(update.unread), update.offered,
        (unsigned)update.phase, (unsigned)update.result, (unsigned long)update.received, (unsigned long)update.total);
    const char *safe_version = strspn(update.installed, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.+-_/") ==
        strlen(update.installed) ? update.installed : "";
    add(request, ",\"installed\":\"%s\"},", safe_version);
    energy_status(request);
    home_status(request);
    add(request, "\"mqtt\":{\"ip\":\"%s\",\"port\":%u,\"tlsName\":\"%s\","
        "\"probePending\":%s,\"probeAttempted\":%s,\"probeResult\":%u,\"probeOn\":%s},",
        snapshot.mqtt_ip, snapshot.mqtt_port, snapshot.mqtt_tls_name,
        truth(snapshot.mqtt_probe_pending), truth(snapshot.mqtt_probe_attempted),
        (unsigned)snapshot.mqtt_probe_result, truth(snapshot.mqtt_probe_on));
    add(request, "\"resources\":{\"uptime\":%u,\"free\":%u,\"minimum\":%u,\"largest\":%u,"
        "\"lowestBrowserLargest\":%u,\"webStack\":%u,\"workerStack\":%u},\"devices\":[",
        (unsigned)snapshot.now, (unsigned)heap_caps_get_free_size(caps),
        (unsigned)heap_caps_get_minimum_free_size(caps), (unsigned)largest, (unsigned)lowest_largest,
        (unsigned)web_stack, (unsigned)snapshot.worker_stack);
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        device_status(request, i);
    }
    add(request, "],\"today\":");
    const dispatch_daily_evidence *today = &snapshot.days[snapshot.local_day % DISPATCH_EVIDENCE_DAYS];
    if (snapshot.clock_valid && snapshot.local_day && today->day == snapshot.local_day) {
        daily_status(request, snapshot.local_day % DISPATCH_EVIDENCE_DAYS);
    } else {
        add(request, "null");
    }
    add(request, ",\"mining\":{\"nodeIp\":\"%s\",\"nodePort\":%u,\"rpcConfigured\":%s,"
        "\"datumIp\":\"%s\",\"datumPort\":%u,\"minerHost\":\"%s\",\"minerPort\":%u,"
        "\"nodeHost\":\"%s\",\"datumHost\":\"%s\"},",
        snapshot.mining.node_ip, snapshot.mining.node_port, truth(snapshot.rpc_configured),
        snapshot.mining.datum_ip, snapshot.mining.datum_port, snapshot.mining.miner_host, snapshot.mining.miner_port,
        snapshot.mining.node_host, snapshot.mining.datum_host);
    add(request, "\"node\":{\"status\":%u,\"blocks\":%u,\"headers\":%u,\"seen\":%s",
        (unsigned)snapshot.node.status, (unsigned)snapshot.node.blocks, (unsigned)snapshot.node.headers, truth(snapshot.node_seen));
    nullable(request, "age", snapshot.node_sampled, snapshot.node_age);
    add(request, "},\"datum\":{\"status\":%u,\"seen\":%s", (unsigned)snapshot.datum.status, truth(snapshot.datum_seen));
    if (snapshot.datum.counters_known) {
        add(request, ",\"accepted\":\"%llu\",\"rejected\":\"%llu\"",
            (unsigned long long)snapshot.datum.accepted, (unsigned long long)snapshot.datum.rejected);
    } else {
        add(request, ",\"accepted\":null,\"rejected\":null");
    }
    nullable(request, "age", snapshot.datum_sampled, snapshot.datum_age);
    uint32_t sync_age = 0;
    const bool seen_sync = network_time_age(&sync_age);
    add(request, "},\"localDay\":%u,\"meterGap\":%s,\"time\":{\"server\":\"%s\",\"localOnly\":%s,\"synchronized\":%s,\"epoch\":%lu",
        (unsigned)snapshot.local_day, truth(snapshot.meter_gap), network_time_source(),
        truth(network_time_local_only()), truth(snapshot.time_synchronized), (unsigned long)network_clock());
    nullable(request, "syncAge", seen_sync, sync_age);
    add(request, "},\"history\":[");
    for (unsigned i = 0; i < snapshot.history_count; ++i) {
        const history_event *event = &snapshot.history[i];
        add(request, "%s{\"at\":%u,\"day\":%u,\"minute\":%u,\"second\":%u,\"device\":%u,"
            "\"previousEquipment\":%s,\"action\":%u,\"text\":\"%s\"", i ? "," : "",
            (unsigned)event->uptime, (unsigned)event->local_day, event->local_minute, event->local_second, event->device,
            truth(event->previous_equipment), event->action, installation_event(event->kind));
        if (event->reason != EVENT_NO_REASON) {
            add(request, ",\"reason\":\"%s\"", dispatch_explanation(event->reason));
        }
        add(request, "}");
    }
    add(request, "]}");
    return json_end(request);
}

static esp_err_t timezones_page(httpd_req_t *request)
{
    if (!authorize(request)) {
        return reply(request, "401 Unauthorized", "Enter the setup or owner key.");
    }
    json_begin(request);
    add(request, "[");
    for (size_t i = 0; output_error == ESP_OK; ++i) {
        const dispatch_timezone *zone = timezone_at(i);
        if (!zone) {
            break;
        }
        add(request, "%s\"%s\"", i ? "," : "", zone->name);
    }
    add(request, "]");
    return json_end(request);
}

static esp_err_t discovery_page(httpd_req_t *request)
{
    if (!authorize(request)) {
        return reply(request, "401 Unauthorized", "Enter the setup or owner key.");
    }
    discovery_read(&discovered);
    json_begin(request);
    add(request, "{\"running\":%s,\"failed\":%s,\"limited\":%s,\"devices\":[",
        truth(discovered.running), truth(discovered.failed), truth(discovered.limited));
    for (unsigned i = 0; i < discovered.count; ++i) {
        const discovery_device *d = &discovered.device[i];
        const bool fresh = d->result == SHELLY_OK && network_uptime() - d->observed_at <= OBSERVATION_TTL;
        add(request, "%s{\"ip\":\"%s\",\"identity\":\"%s\",\"outlet\":%s,"
            "\"locked\":%s,\"selectable\":%s", i ? "," : "", d->ip, d->identity,
            fresh ? truth(d->sample.on) : "null", truth(d->result == SHELLY_AUTH),
            truth(d->result == SHELLY_OK || d->result == SHELLY_AUTH));
        nullable(request, "watts", fresh && d->sample.power_known, d->sample.watts);
        add(request, "}");
    }
    add(request, "]}");
    return json_end(request);
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool decoded(const char *from, const char *to, char *out, size_t cap)
{
    size_t used = 0;
    while (from < to) {
        int ch = (unsigned char)*from++;
        if (ch == '%') {
            if (to - from < 2 || hex(from[0]) < 0 || hex(from[1]) < 0) {
                return false;
            }
            ch = (hex(from[0]) << 4) | hex(from[1]);
            from += 2;
        } else if (ch == '+') {
            ch = ' ';
        }
        if (used >= cap - 1 || ch < 32 || ch == 127) {
            return false;
        }
        out[used++] = ch;
    }
    out[used] = 0;
    return true;
}

typedef enum { FIELD_INVALID, FIELD_MISSING, FIELD_PRESENT } form_field;

static form_field read_field(const char *name, char *out, size_t cap)
{
    size_t len = strlen(name);
    bool found = false;
    for (const char *part = form; *part; ) {
        const char *end = part + strcspn(part, "&");
        if ((size_t)(end - part) > len && part[len] == '=' && !memcmp(part, name, len)) {
            if (found || !decoded(part + len + 1, end, out, cap)) {
                return FIELD_INVALID;
            }
            found = true;
        }
        part = *end ? end + 1 : end;
    }
    return found ? FIELD_PRESENT : FIELD_MISSING;
}

static bool field(const char *name, char *out, size_t cap)
{
    return read_field(name, out, cap) == FIELD_PRESENT;
}

static bool integer(const char *name, long min, long max, int *out)
{
    char value[12], *end;
    if (!field(name, value, sizeof(value)) || !value[0] ||
        (value[0] != '-' && !isdigit((unsigned char)value[0]))) {
        return false;
    }
    long number = strtol(value, &end, 10);
    if (*end || number < min || number > max) {
        return false;
    }
    *out = number;
    return true;
}

static bool endpoint(const char *ip)
{
    struct in_addr address;
    if (inet_pton(AF_INET, ip, &address) != 1) {
        return false;
    }
    uint32_t a = ntohl(address.s_addr);
    return (a >> 24 == 10 || a >> 20 == 0xac1 || a >> 16 == 0xc0a8);
}

static esp_err_t save_wifi(void)
{
    network_settings settings = {0};
    bool valid = field("ssid", settings.ssid, sizeof(settings.ssid)) && settings.ssid[0] &&
        field("password", settings.password, sizeof(settings.password)) &&
        (settings.password[0] == 0 || (strlen(settings.password) >= 8 &&
        (strlen(settings.password) < 64 || strspn(settings.password, "0123456789abcdefABCDEF") == 64))) &&
        field("owner", settings.owner_key, sizeof(settings.owner_key)) &&
        network_valid_key(settings.owner_key) &&
        field("ntp", settings.ntp, sizeof(settings.ntp)) && network_valid_ntp(settings.ntp);
    esp_err_t result = valid ? network_store(&settings) : ESP_ERR_INVALID_ARG;
    explicit_bzero(&settings, sizeof(settings));
    return result;
}

static const char *read_rate_windows(dispatch_tariff *tariff, bool weekend)
{
    const char *const fields[4] = {"Enabled", "Start", "End", "Price"};
    const int maximum[4] = {1, 1439, 1439, DISPATCH_RATE_MAX};
    for (unsigned i = 0; i < DISPATCH_RATE_WINDOWS; ++i) {
        int values[4];
        for (unsigned field = 0; field < 4; ++field) {
            char key[24];
            snprintf(key, sizeof(key), "%srate%u%s", weekend ? "w" : "", i + 1, fields[field]);
            if (!integer(key, 0, maximum[field], &values[field])) {
                return "Enter valid times and prices for the rate windows.";
            }
        }
        tariff->window[i] = (dispatch_rate_window){values[1], values[2], values[3], values[0]};
    }
    return dispatch_tariff_valid(tariff) ? NULL : "Enabled rate windows need different start and end times and cannot overlap.";
}

static const char *read_weekend(dispatch_weekend_plan *weekend)
{
    char enabled[2];
    const form_field flag = read_field("weekend", enabled, sizeof(enabled));
    if (flag == FIELD_PRESENT && !strcmp(enabled, "0")) return NULL;
    int start, end, quiet, quiet_start, quiet_end;
    if (flag != FIELD_PRESENT || strcmp(enabled, "1") ||
        !integer("wstart", 0, 1439, &start) || !integer("wend", 0, 1439, &end) ||
        !integer("wquiet", 0, 1, &quiet) || !integer("wquietStart", 0, 1439, &quiet_start) ||
        !integer("wquietEnd", 0, 1439, &quiet_end) || (quiet && quiet_start == quiet_end)) {
        return "Check the weekend schedule and Quiet Hours.";
    }
    weekend->start_minute = start;
    weekend->end_minute = end;
    weekend->enabled = true;
    weekend->quiet = (dispatch_quiet_hours){quiet_start, quiet_end, quiet};
    return read_rate_windows(&weekend->tariff, true);
}

typedef struct {
    dispatch_policy policy;
    dispatch_quiet_hours quiet;
    dispatch_tariff tariff;
    dispatch_weekend_plan weekend;
    const dispatch_timezone *timezone;
    int offset;
} config_plan;

static const char *read_plan(config_plan *plan, bool importing)
{
    int start, end, rate, allowance, quiet, quiet_start, quiet_end;
    char name[TIMEZONE_NAME_BYTES];
    if (!integer("start", 0, 1439, &start) || !integer("end", 0, 1439, &end)) {
        return "Enter valid start and end times.";
    }
    if (!integer("quiet", 0, 1, &quiet) ||
        !integer("quietStart", 0, 1439, &quiet_start) || !integer("quietEnd", 0, 1439, &quiet_end) ||
        (quiet && quiet_start == quiet_end)) {
        return "Enter valid Quiet Hours settings.";
    }
    plan->quiet = (dispatch_quiet_hours){quiet_start, quiet_end, quiet};
    if (!field("timezone", name, sizeof(name)) || (name[0] && !(plan->timezone = timezone_find(name))) ||
        (importing && !integer("offset", -720, 840, &plan->offset))) {
        return "Select a supported time zone or fixed offset.";
    }
    if (!integer("rate", 0, DISPATCH_RATE_MAX, &rate) ||
        !integer("allowance", 0, 1000000000, &allowance)) {
        return "Check the electricity rate and daily allowance.";
    }
    const char *tariff_error = read_rate_windows(&plan->tariff, false);
    if (tariff_error) return tariff_error;
    if (allowance && !dispatch_highest_rate(rate, &plan->tariff)) {
        return "Enter a positive electricity rate to use a daily allowance.";
    }
    const char *weekend_error = read_weekend(&plan->weekend);
    if (weekend_error) return weekend_error;
    if (plan->weekend.enabled && allowance && !dispatch_highest_rate(rate, &plan->weekend.tariff)) {
        return "Enter a positive electricity rate for the weekend allowance.";
    }
    plan->policy = (dispatch_policy){start, end, rate, allowance};
    return NULL;
}

static const char *read_association(association *a, const char *ip, const char *identity, const char *miner)
{
    if (!field(ip, a->shelly.ip, sizeof(a->shelly.ip)) ||
        (a->shelly.ip[0] && !endpoint(a->shelly.ip))) {
        return "Enter a private IPv4 address for the outlet.";
    }
    if (!field(miner, a->miner_ip, sizeof(a->miner_ip)) || (a->miner_ip[0] && !endpoint(a->miner_ip))) {
        return "Enter a private IPv4 address for cgminer or leave it blank.";
    }
    if (!field(identity, a->shelly.identity, sizeof(a->shelly.identity)) ||
        (a->shelly.ip[0] && outlet_identity(a->shelly.identity) == OUTLET_INVALID)) {
        return "Enter a supported Shelly ID, Tasmota MAC and channel, or Home Assistant switch entity.";
    }
    if (!a->shelly.ip[0] && (a->shelly.identity[0] || a->miner_ip[0])) {
        return "Enter the outlet address or clear the device ID and cgminer address.";
    }
    return NULL;
}

static esp_err_t read_operating(operating_settings *operating, const char **error)
{
    int paid, from, until, weekend_from, weekend_until, bridge;
    *error = "Check the paid mining window and daylight gap settings.";
    if (!integer("paidEnabled", 0, 1, &paid) || !integer("paidStart", 0, 1439, &from) ||
        !integer("paidEnd", 0, 1439, &until) ||
        !integer("weekendPaidStart", 0, 1439, &weekend_from) ||
        !integer("weekendPaidEnd", 0, 1439, &weekend_until) ||
        !integer("bridgeEnabled", 0, 1, &bridge))
        return ESP_ERR_INVALID_ARG;
    operating->plan = (dispatch_operating_plan){.current_format = true, .paid_enabled = paid,
        .paid_start = from, .paid_end = until, .weekend_paid_start = weekend_from, .weekend_paid_end = weekend_until};
    operating->daylight.enabled = bridge;
    if (bridge) {
        int latitude, longitude, minutes;
        if (!integer("latitudeE4", -900000, 900000, &latitude) ||
            !integer("longitudeE4", -1800000, 1800000, &longitude) ||
            !integer("bridgeMinutes", BRIDGE_MIN_MINUTES, BRIDGE_MAX_MINUTES, &minutes) ||
            !dispatch_solar_location_valid(latitude, longitude)) return ESP_ERR_INVALID_ARG;
        operating->daylight.latitude_e4 = latitude;
        operating->daylight.longitude_e4 = longitude;
        operating->daylight.minutes = minutes;
    }
    return ESP_OK;
}

static esp_err_t save_installation(const char **error, operating_scenario *scenarios)
{
    char scope[8];
    const form_field scope_result = read_field("scope", scope, sizeof(scope));
    const bool pair_only = scope_result == FIELD_PRESENT && !strcmp(scope, "pair");
    const bool plan_only = scope_result == FIELD_PRESENT &&
        !strcmp(scope, scenarios ? "whatif" : "plan");
    if (scenarios && !plan_only) return ESP_ERR_INVALID_ARG;
    if (!pair_only && !plan_only) {
        *error = "Select a supported settings section.";
        return ESP_ERR_INVALID_ARG;
    }

    if (plan_only) {
        config_plan plan = {0};
        const char *issue = read_plan(&plan, false);
        if (issue) {
            *error = issue;
            return ESP_ERR_INVALID_ARG;
        }
        operating_settings operating = {0};
        if (read_operating(&operating, error) != ESP_OK) return ESP_ERR_INVALID_ARG;
        if (scenarios) {
            char option[2];
            const form_field supplied = read_field("reviewWeekend", option, sizeof(option));
            const bool review_weekend = supplied == FIELD_PRESENT && !strcmp(option, "1");
            if (supplied != FIELD_MISSING && (!review_weekend || !plan.weekend.enabled)) return ESP_ERR_INVALID_ARG;
            if (review_weekend) {
                plan.policy.start_minute = plan.weekend.start_minute;
                plan.policy.end_minute = plan.weekend.end_minute;
                operating.plan.paid_start = operating.plan.weekend_paid_start;
                operating.plan.paid_end = operating.plan.weekend_paid_end;
            }
            installation_preview_plan(&plan.policy, review_weekend ? &plan.weekend.quiet : &plan.quiet,
                review_weekend ? &plan.weekend.tariff : &plan.tariff, &operating, scenarios);
            return ESP_OK;
        }
        return installation_set_plan(&plan.policy, &plan.quiet, &plan.tariff, &plan.weekend, plan.timezone,
                                     &operating);
    }

    association assoc = {0};
    char auth[8];
    int index;
    const char *issue = read_association(&assoc, "ip", "identity", "miner");
    if (!integer("index", 0, DISPATCH_LIMIT - 1, &index)) {
        issue = "Select a miner association.";
    } else if (!issue && (!field("auth", auth, sizeof(auth)) ||
        !field("password", assoc.shelly.password, sizeof(assoc.shelly.password)) ||
        !((!strcmp(auth, "set") && assoc.shelly.ip[0] && assoc.shelly.password[0] &&
            outlet_identity(assoc.shelly.identity) != OUTLET_HA &&
            outlet_identity(assoc.shelly.identity) != OUTLET_MQTT && outlet_valid_password(&assoc.shelly)) ||
         ((!strcmp(auth, "keep") || !strcmp(auth, "none")) && !assoc.shelly.password[0])))) {
        issue = "Enter the credential for this outlet type, or leave it blank.";
    }
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (issue) {
        *error = issue;
    } else {
        result = installation_configure(index, &assoc, !strcmp(auth, "keep"));
        if (result == ESP_ERR_INVALID_ARG) {
            *error = "Another slot already uses this outlet channel, entity, or miner address.";
        }
    }
    explicit_bzero(&assoc, sizeof(assoc));
    return result;
}

static esp_err_t import_installation(const char **error)
{
    int version;
    if (!integer("version", 3, 4, &version)) {
        *error = "Unsupported configuration file version.";
        return ESP_ERR_INVALID_ARG;
    }
    config_plan plan = {0};
    const char *issue = read_plan(&plan, true);
    if (issue) {
        *error = issue;
        return ESP_ERR_INVALID_ARG;
    }
    operating_settings operating = {0};
    if (read_operating(&operating, error) != ESP_OK) return ESP_ERR_INVALID_ARG;
    association devices[DISPATCH_LIMIT] = {0};
    const unsigned slots = version == 3 ? 4 : DISPATCH_LIMIT;
    for (unsigned i = 0; i < slots; ++i) {
        char ip[] = "ip0", identity[] = "identity0", miner[] = "miner0";
        ip[2] += i; identity[8] += i; miner[5] += i;
        issue = read_association(&devices[i], ip, identity, miner);
        if (issue) {
            *error = issue;
            return ESP_ERR_INVALID_ARG;
        }
    }
    const esp_err_t result = installation_import(&plan.policy, &plan.quiet, &plan.tariff, &plan.weekend,
                                                  plan.timezone, plan.offset, devices, &operating);
    if (result == ESP_ERR_INVALID_ARG) {
        *error = "Imported outlets, IDs, and cgminer addresses must be unique across slots.";
    }
    return result;
}

static bool host_field(const char *field_name, char *host, size_t size)
{
    return field(field_name, host, size) &&
        strspn(host, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-") == strlen(host);
}

static esp_err_t save_solar(const char **error)
{
    solar_settings settings = {0};
    int port = 0, start, end;
    char source[8] = "ecu";
    const form_field supplied = read_field("source", source, sizeof(source));
    const bool bridge = supplied == FIELD_PRESENT && !strcmp(source, "bridge");
    *error = "Check the solar source IPv4 address, gateway ID, port, and polling times.";
    if ((supplied != FIELD_MISSING && (supplied != FIELD_PRESENT || (!bridge && strcmp(source, "ecu")))) ||
        !field("ip", settings.ip, sizeof(settings.ip)) || (settings.ip[0] && !endpoint(settings.ip)) ||
        !field("id", settings.id, sizeof(settings.id)) || (settings.id[0] &&
        (strlen(settings.id) != 12 || strspn(settings.id, "0123456789") != 12)) ||
        (bridge && settings.ip[0] && !settings.id[0]) ||
        (!bridge && !integer("port", 1, 65535, &port)) ||
        !integer("nightStart", 0, 1439, &start) || !integer("nightEnd", 0, 1439, &end)) return ESP_ERR_INVALID_ARG;
    settings.port = port;
    settings.night_start = start;
    settings.night_end = end;
    return installation_solar(&settings);
}

static bool read_start_watts(uint16_t watts[DISPATCH_LIMIT])
{
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        char key[] = "watts0";
        key[5] += i;
        int value;
        if (!integer(key, 0, 10000, &value)) return false;
        watts[i] = value;
    }
    return true;
}

static esp_err_t save_follow(const char **error, bool policy_only)
{
    follow_settings settings = {0};
    int reserve, margin, restart, shed;
    *error = "Check the power reserves, preset margin, and timers.";
    if (!integer("reserve", 0, 50000, &reserve) || !integer("margin", 0, 10000, &margin) ||
        !integer("restartMinutes", 0, FOLLOW_TIMER_LIMIT_MINUTES, &restart) ||
        !integer("shedMinutes", 0, FOLLOW_TIMER_LIMIT_MINUTES, &shed)) return ESP_ERR_INVALID_ARG;
    settings.reserve = reserve;
    settings.shed_margin = margin;
    settings.restart_seconds = restart * 60;
    settings.shed_delay_seconds = shed * 60;
    if (policy_only) {
        int grid_reserve;
        if (!integer("gridReserve", 0, 10000, &grid_reserve)) return ESP_ERR_INVALID_ARG;
        return installation_power_policy(&settings, grid_reserve);
    }
    if (!read_start_watts(settings.start_watts)) {
        *error = "Check the startup wattage.";
        return ESP_ERR_INVALID_ARG;
    }
    return installation_follow(&settings);
}

static bool read_native_grid(grid_settings *settings, bool sunspec)
{
    char serial[33];
    int port, unit, address, reverse;
    if (!field("serial", serial, sizeof(serial)) || !integer("port", 1, 65535, &port) ||
        !integer("unit", 1, 247, &unit) || !integer("address", 0, 65535, &address) ||
        !integer("reverse", 0, 1, &reverse)) return false;
    settings->native = (native_grid_config){.port = port, .unit = unit, .address = address};
    settings->reverse = reverse;
    if (sunspec) {
        int model, index;
        if (!integer("model", 201, 214, &model) || !integer("meterIndex", 1, 16, &index)) return false;
        settings->native.model = model;
        settings->native.meter_index = index;
    } else {
        int function, exponent;
        if (!integer("function", 3, 4, &function) || !integer("exponent", -6, 6, &exponent)) return false;
        settings->native.function = function;
        settings->native.exponent = exponent;
    }
    snprintf(settings->target.identity, sizeof(settings->target.identity), "%s%s", sunspec ? "sun:" : "sv5:", serial);
    return native_grid_valid(settings->target.identity, &settings->native);
}

static esp_err_t save_grid(const char **error)
{
    grid_settings settings = {0};
    int verified, reserve = 0;
    char reserve_field[12], source[12] = "legacy";
    const bool keep_reserve = read_field("reserve", reserve_field, sizeof(reserve_field)) == FIELD_MISSING;
    const form_field supplied = read_field("source", source, sizeof(source));
    const bool sunspec = !strcmp(source, "sunspec");
    const bool native = sunspec || !strcmp(source, "solarman-v5");
    *error = "Enter a valid whole-site meter address and setup. Surplus control requires verified meter direction.";
    bool valid = supplied != FIELD_INVALID && (native || !strcmp(source, "legacy")) &&
        field("ip", settings.target.ip, sizeof(settings.target.ip)) &&
        integer("verified", 0, 1, &verified) && (keep_reserve || integer("reserve", 0, 10000, &reserve));
    bool keep = false;
    if (valid && !settings.target.ip[0]) {
        valid = !verified;
    } else if (valid && native) {
        valid = endpoint(settings.target.ip) && read_native_grid(&settings, sunspec);
    } else if (valid) {
        char auth[5];
        valid = endpoint(settings.target.ip) &&
            field("identity", settings.target.identity, sizeof(settings.target.identity)) &&
            installation_grid_identity(settings.target.identity) &&
            field("auth", auth, sizeof(auth)) && field("password", settings.target.password, sizeof(settings.target.password));
        if (valid) {
            keep = !strcmp(auth, "keep");
            valid = !strcmp(auth, "set") ?
                grid_shelly_identity(settings.target.identity) && settings.target.password[0] &&
                    outlet_valid_password(&settings.target) :
                (keep || !strcmp(auth, "none")) && !settings.target.password[0];
        }
    }
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (valid) {
        settings.direction_verified = verified;
        settings.reserve = reserve;
        result = installation_grid(&settings, keep, keep_reserve);
    }
    explicit_bzero(&settings, sizeof(settings));
    return result;
}

static bool optional_sensor(const char *name, char entity[HA_ENTITY_BYTES])
{
    return field(name, entity, HA_ENTITY_BYTES) && (!entity[0] || ha_valid_entity(entity, HA_SENSOR));
}

static esp_err_t save_mqtt(const char **error)
{
    char auth[5];
    int port;
    *error = "Enter a private broker address, port and credentials. For TLS, supply the broker DNS name and CA certificate.";
    mqtt_endpoint *settings = calloc(1, sizeof(*settings));
    if (!settings) return ESP_ERR_NO_MEM;
    bool valid = field("ip", settings->ip, sizeof(settings->ip)) && integer("port", 1, 65535, &port) &&
        field("auth", auth, sizeof(auth)) && field("name", settings->tls_name, sizeof(settings->tls_name)) &&
        field("user", settings->user, sizeof(settings->user)) &&
        field("password", settings->password, sizeof(settings->password)) &&
        field("ca", settings->ca, sizeof(settings->ca));
    const bool keep = valid && !strcmp(auth, "keep") && settings->ip[0];
    if (valid) {
        const bool has_secret = settings->user[0] || settings->password[0] || settings->ca[0];
        valid = (!settings->tls_name[0] || mqtt_valid_name(settings->tls_name)) &&
            (settings->ip[0] ? endpoint(settings->ip) : !settings->tls_name[0]);
        if (valid && settings->ip[0] && !keep) {
            valid = !strcmp(auth, "set") && settings->user[0] && settings->password[0] &&
                (settings->tls_name[0] ? mqtt_valid_ca(settings->ca) : !settings->ca[0]);
        } else if (valid) {
            valid = !has_secret;
        }
    }
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (valid) {
        settings->port = settings->ip[0] ? port : 0;
        result = installation_mqtt(settings, keep);
    }
    explicit_bzero(settings, sizeof(*settings));
    free(settings);
    return result;
}

static esp_err_t save_home(const char **error)
{
    home_settings settings = {0};
    char auth[5];
    int port;
    *error = "Enter the Home Assistant LAN address, port, token, and any selected sensor or control entity IDs.";
    bool valid = field("ip", settings.endpoint.ip, sizeof(settings.endpoint.ip)) &&
        integer("port", 1, 65535, &port) &&
        field("auth", auth, sizeof(auth)) &&
        field("token", settings.endpoint.token, sizeof(settings.endpoint.token)) &&
        optional_sensor("battery", settings.battery) && optional_sensor("temperature", settings.temperature) &&
        field("actuator", settings.actuator, sizeof(settings.actuator)) &&
        (!settings.actuator[0] || ha_valid_entity(settings.actuator, HA_NUMBER) ||
            ha_valid_entity(settings.actuator, HA_SELECT) || ha_valid_entity(settings.actuator, HA_BUTTON));
    if (valid && settings.endpoint.ip[0]) {
        valid = endpoint(settings.endpoint.ip) &&
            (!strcmp(auth, "keep") ? !settings.endpoint.token[0] :
                !strcmp(auth, "set") && ha_valid_token(settings.endpoint.token));
    } else if (valid) {
        valid = !settings.endpoint.token[0] && !settings.battery[0] && !settings.temperature[0] &&
            !settings.actuator[0];
    }
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (valid) {
        settings.endpoint.port = settings.endpoint.ip[0] ? port : 0;
        result = installation_ha(&settings, settings.endpoint.ip[0] && !strcmp(auth, "keep"));
    }
    explicit_bzero(&settings, sizeof(settings));
    return result;
}

static esp_err_t save_mining(const char **error)
{
    mining_settings settings = {0};
    int node_port, datum_port, miner_port;
    const char *issue = NULL;
    if (!field("nodeIp", settings.node_ip, sizeof(settings.node_ip)) ||
        (settings.node_ip[0] && !endpoint(settings.node_ip)) ||
        !integer("nodePort", 1, 65535, &node_port) ||
        !host_field("nodeHost", settings.node_host, sizeof(settings.node_host))) {
        issue = "Enter a private node IPv4 address and an RPC port, or clear the address.";
    } else if (!field("rpcUser", settings.rpc_user, sizeof(settings.rpc_user)) ||
        !field("rpcPassword", settings.rpc_password, sizeof(settings.rpc_password)) ||
        (!!settings.rpc_user[0] != !!settings.rpc_password[0]) || strchr(settings.rpc_user, ':') ||
        (!settings.node_ip[0] && settings.rpc_user[0])) {
        issue = "Enter the RPC username and password together.";
    } else if (!field("datumIp", settings.datum_ip, sizeof(settings.datum_ip)) ||
        (settings.datum_ip[0] && !endpoint(settings.datum_ip)) ||
        !integer("datumPort", 1, 65535, &datum_port) ||
        !host_field("datumHost", settings.datum_host, sizeof(settings.datum_host))) {
        issue = "Enter a private DATUM dashboard IPv4 address and port, or clear the address.";
    } else if (!host_field("minerHost", settings.miner_host, sizeof(settings.miner_host)) ||
        !integer("minerPort", 1, 65535, &miner_port)) {
        issue = "Enter the miner-facing DATUM host and port without a protocol or path.";
    }
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (issue) {
        *error = issue;
    } else {
        settings.node_port = node_port;
        settings.datum_port = datum_port;
        settings.miner_port = miner_port;
        if (!settings.datum_ip[0]) settings.miner_host[0] = 0;
        else if (!settings.miner_host[0]) strcpy(settings.miner_host, settings.datum_ip);
        result = installation_mining(&settings);
        if (result == ESP_ERR_INVALID_ARG) *error = "Enter credentials for this node endpoint.";
    }
    explicit_bzero(&settings, sizeof(settings));
    return result;
}

static bool valid_equipment_name(const char *name)
{
    const unsigned char *byte = (const unsigned char *)name;
    if (*byte == ' ') return false;
    while (*byte) {
        uint32_t code = *byte++;
        if (code < 0x20 || code == 0x7f) return false;
        if (code < 0x80) continue;
        unsigned remaining;
        if (code >= 0xc2 && code <= 0xdf) {
            code &= 0x1f;
            remaining = 1;
        } else if (code >= 0xe0 && code <= 0xef) {
            code &= 0x0f;
            remaining = 2;
        } else if (code >= 0xf0 && code <= 0xf4) {
            code &= 0x07;
            remaining = 3;
        } else return false;
        const uint32_t minimum = remaining == 1 ? 0xa0 : remaining == 2 ? 0x800 : 0x10000;
        while (remaining--) {
            if ((*byte & 0xc0) != 0x80) return false;
            code = (code << 6) | (*byte++ & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return byte == (const unsigned char *)name || byte[-1] != ' ';
}

static esp_err_t apply_action(const char **conflict, const char **error)
{
    char mode[16];
    if (!field("mode", mode, sizeof(mode))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(mode, "update-enable")) {
        int enabled;
        *conflict = "Network Update is unavailable or already working.";
        return integer("enabled", 0, 1, &enabled) ? network_update_enable(enabled != 0) : ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(mode, "update-check")) {
        *conflict = "Enable Network Update, wait for Wi-Fi and clock sync, then allow 15 seconds between checks.";
        return network_update_check();
    }
    if (!strcmp(mode, "update-seen") || !strcmp(mode, "update-install")) {
        char version[32];
        *conflict = "Wait for a current update offer and for any update work to finish.";
        if (!field("version", version, sizeof(version))) return ESP_ERR_INVALID_ARG;
        return !strcmp(mode, "update-seen") ? network_update_seen(version) : network_update_install(version);
    }
    if (!strcmp(mode, "solar-ecu")) {
        *conflict = "Check the saved bridge address, or turn off Follow solar generation before removing the source.";
        return save_solar(error);
    }
    if (!strcmp(mode, "solar-follow")) {
        *conflict = "Save a solar source first. Automatic solar presets require Efficiency mode and preset evaluation to be off.";
        return save_follow(error, false);
    }
    if (!strcmp(mode, "solar-policy")) return save_follow(error, true);
    if (!strcmp(mode, "solar-start")) {
        int slot, identity, watts;
        *error = "Enter a startup draw from 0 to 10,000 watts.";
        *conflict = "The selected pairing has changed. Reload it before saving startup draw.";
        if (!integer("slot", 0, DISPATCH_LIMIT - 1, &slot) ||
            !integer("identity", 1, UINT16_MAX, &identity) || !integer("watts", 0, 10000, &watts))
            return ESP_ERR_INVALID_ARG;
        return installation_startup_draw(slot, identity, watts);
    }
    if (!strcmp(mode, "solar-meter")) {
        *conflict = "Save the meter with surplus control off first and verify a fresh grid reading before enabling surplus control.";
        return save_grid(error);
    }
    if (!strcmp(mode, "mqtt-probe")) {
        char identity[40];
        *error = "Enter a Tasmota MQTT device ID with its MAC and exact topic.";
        *conflict = "Save a broker and wait for any pending test before trying again.";
        if (!field("identity", identity, sizeof(identity))) return ESP_ERR_INVALID_ARG;
        return installation_mqtt_probe(identity);
    }
    if (!strcmp(mode, "home-config")) {
        *conflict = "Keep the saved host and port for an existing token, or remove Home Assistant outlet and meter mappings before replacing the connection.";
        return save_home(error);
    }
    if (!strcmp(mode, "home-meter")) {
        int slot, identity;
        home_meter_mapping mapping = {0};
        *error = "Enter the optional power and cumulative energy sensor entity IDs.";
        *conflict = "Select an outlet paired to Home Assistant before editing its meter mapping.";
        if (!integer("slot", 0, DISPATCH_LIMIT - 1, &slot) || !integer("identity", 1, UINT16_MAX, &identity) ||
            !optional_sensor("power", mapping.power) || !optional_sensor("energy", mapping.energy) ||
            (mapping.power[0] && !strcmp(mapping.power, mapping.energy))) return ESP_ERR_INVALID_ARG;
        return installation_ha_meter(slot, identity, &mapping);
    }
    if (!strcmp(mode, "home-action")) {
        char entity[HA_ENTITY_BYTES], option[HA_OPTION_BYTES], number[32], *end;
        *error = "Choose a valid value for the configured Home Assistant entity.";
        *conflict = "Wait for a fresh Home Assistant observation and resume operation before requesting another change.";
        if (!field("entity", entity, sizeof(entity)) || !field("option", option, sizeof(option)) ||
            !field("value", number, sizeof(number))) return ESP_ERR_INVALID_ARG;
        const double value = strtod(number, &end);
        if (*end || end == number || !isfinite(value) || fabs(value) > 1e9) return ESP_ERR_INVALID_ARG;
        return installation_ha_action(entity, value, option);
    }
    if (!strcmp(mode, "gx-target")) {
        char ip[16];
        int expected, watts;
        *error = "Enter a grid target between -10000 W and 10000 W.";
        *conflict = "Verify meter direction, wait for fresh grid and target readings, and resume operation before applying a target.";
        if (!field("ip", ip, sizeof(ip)) || !endpoint(ip) ||
            !integer("expected", INT32_MIN, INT32_MAX, &expected) ||
            !integer("watts", -VICTRON_TARGET_MAX, VICTRON_TARGET_MAX, &watts)) return ESP_ERR_INVALID_ARG;
        return installation_gx_target(ip, expected, watts);
    }
    if (!strcmp(mode, "time-retry")) {
        *conflict = "Clock retry requires an active network and a configured time source.";
        return network_retry_time();
    }
    if (!strcmp(mode, "power-test")) {
        int slot, id, start, confirmed;
        *error = "Select the currently paired outlet and confirm the timed activation.";
        if (!integer("slot", 0, DISPATCH_LIMIT - 1, &slot) ||
            !integer("id", 1, UINT16_MAX, &id) || !integer("start", 0, 1, &start) ||
            (start && !integer("confirm", 1, 1, &confirmed))) return ESP_ERR_INVALID_ARG;
        return installation_power_test(slot, id, start, conflict);
    }
    if (!strcmp(mode, "time-config")) {
        char ntp[64];
        int local_only;
        *conflict = "Clock settings require an active network connection.";
        *error = "Enter an NTP hostname or IPv4 address. Local-only mode needs a server.";
        if (!field("ntp", ntp, sizeof(ntp)) || !integer("localOnly", 0, 1, &local_only)) {
            return ESP_ERR_INVALID_ARG;
        }
        return network_store_time(ntp, local_only != 0);
    }
    if (!strcmp(mode, "resume-meter")) {
        *conflict = "Meter recovery needs a synchronized clock and a fresh energy reading from each affected outlet.";
        return installation_accounting(false);
    }
    if (!strcmp(mode, "clear-account")) {
        int confirmed;
        *error = "Confirm that you want to clear electricity accounting and daily reports.";
        return integer("confirm", 1, 1, &confirmed) ? installation_accounting(true) : ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(mode, "mining")) return save_mining(error);
    if (!strcmp(mode, "notice-config")) {
        notification_target target = {0};
        int port;
        *error = "Enter a private IPv4 address, port, and a valid webhook path. A saved path can be kept at the same address and port.";
        const bool valid = field("ip", target.ip, sizeof(target.ip)) && endpoint(target.ip) &&
            integer("port", 1, 65535, &port) && field("path", target.path, sizeof(target.path)) &&
            (!target.path[0] || notification_path_valid(target.path));
        if (valid) target.port = port;
        const esp_err_t result = valid ? installation_notifications(&target) : ESP_ERR_INVALID_ARG;
        explicit_bzero(&target, sizeof(target));
        return result;
    }
    if (!strcmp(mode, "notice-off")) {
        const notification_target target = {0};
        return installation_notifications(&target);
    }
    if (!strcmp(mode, "notice-test")) {
        *conflict = "Save a webhook first and wait for any pending test or queued alerts.";
        return installation_notification_test();
    }
    if (!strcmp(mode, "preset-allow")) {
        int slot;
        char password[GOLDSHELL_PASSWORD_BYTES] = {0};
        *conflict = "Authorization requires a fresh, powered miner with a hardware-tested Default and reduced preset.";
        *error = "Enter the miner dashboard password.";
        const bool valid = integer("slot", 0, DISPATCH_LIMIT - 1, &slot) &&
            field("password", password, sizeof(password)) && password[0];
        const esp_err_t result = valid ? installation_preset_authorize(slot, password) : ESP_ERR_INVALID_ARG;
        explicit_bzero(password, sizeof(password));
        return result;
    }
    if (!strcmp(mode, "preset-revoke")) {
        int slot;
        return integer("slot", 0, DISPATCH_LIMIT - 1, &slot) ? installation_preset_revoke(slot) : ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(mode, "preset-eval")) {
        int slot, start;
        *conflict = "Evaluation needs a running authorized miner, an incomplete comparison, and Manual preset control.";
        return integer("slot", 0, DISPATCH_LIMIT - 1, &slot) && integer("start", 0, 1, &start) ?
            installation_preset_evaluate(slot, start) : ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(mode, "preset-choice")) {
        int slot, identity, choice;
        *conflict = "Check the current outlet mapping, preset authorization, and efficiency evaluation.";
        return integer("slot", 0, DISPATCH_LIMIT - 1, &slot) &&
            integer("identity", 1, UINT16_MAX, &identity) &&
            integer("choice", PRESET_MANUAL, PRESET_EFFICIENCY, &choice) ?
            installation_preset_choice(slot, identity, choice) : ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(mode, "miner-restart")) {
        int slot, identity;
        char password[GOLDSHELL_PASSWORD_BYTES] = {0};
        *error = "Choose the current miner and provide its dashboard password.";
        *conflict = "Wait for a current miner and outlet reading in an allowed operating period.";
        const bool valid = integer("slot", 0, DISPATCH_LIMIT - 1, &slot) &&
            integer("identity", 1, UINT16_MAX, &identity) && field("password", password, sizeof(password)) && password[0];
        const esp_err_t result = valid ? installation_miner_restart(slot, identity, password) : ESP_ERR_INVALID_ARG;
        explicit_bzero(password, sizeof(password));
        return result;
    }
    if (!strcmp(mode, "goldshell-scan") || !strcmp(mode, "goldshell-mode")) {
        int slot, expected = GOLDSHELL_MODE_UNKNOWN, desired = GOLDSHELL_MODE_UNKNOWN;
        char password[GOLDSHELL_PASSWORD_BYTES] = {0};
        const bool inspect = !strcmp(mode, "goldshell-scan");
        *conflict = inspect ? "Power-mode inspection requires a fresh powered outlet and an idle request queue." :
            "Inspect the current preset again or wait for the miner to be running under outlet control.";
        *error = "Select a supported preset and enter the miner password.";
        const bool valid = integer("slot", 0, DISPATCH_LIMIT - 1, &slot) &&
            (inspect || (integer("expected", GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_IDLE, &expected) &&
                integer("desired", GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_IDLE, &desired) &&
                         field("password", password, sizeof(password)) && password[0]));
        const esp_err_t result = valid ? installation_power_mode(slot, expected, desired, password) : ESP_ERR_INVALID_ARG;
        explicit_bzero(password, sizeof(password));
        return result;
    }
    if (!strcmp(mode, "rename")) {
        int slot;
        char identity[sizeof(((shelly_target *)0)->identity)], name[EQUIPMENT_NAME_BYTES];
        *error = "Enter a display name up to 48 UTF-8 bytes, or clear the name to reset it.";
        *conflict = "The outlet in this slot changed. Reopen Equipment before renaming it.";
        if (!integer("slot", 0, DISPATCH_LIMIT - 1, &slot) ||
            !field("identity", identity, sizeof(identity)) ||
            !field("name", name, sizeof(name)) || !valid_equipment_name(name)) {
            return ESP_ERR_INVALID_ARG;
        }
        return installation_rename(slot, identity, name);
    }
    if (!strcmp(mode, "coordinate")) {
        int slot, priority, recovery;
        if (integer("slot", 0, DISPATCH_LIMIT - 1, &slot) && integer("priority", 0, 2, &priority) &&
            integer("recovery", 0, 1, &recovery)) {
            *error = "Choose a paired outlet. Automatic recovery also requires cgminer.";
            return installation_coordination(slot, priority, recovery);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(mode, "override")) {
        int slot, choice, minutes;
        *conflict = "A paired outlet and synchronized clock are required for an override.";
        if (integer("slot", 0, DISPATCH_LIMIT - 1, &slot) &&
            integer("choice", OVERRIDE_NONE, OVERRIDE_PREFER, &choice) && integer("minutes", 1, 120, &minutes)) {
            return installation_override(slot, choice, minutes * 60);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(mode, "discover")) {
        *conflict = "Discovery is unavailable while offline or for 10 seconds after a scan.";
        return discovery_request();
    }
    if (!strcmp(mode, "confirm-map")) {
        int slot;
        association expected = {0};
        *error = read_association(&expected, "ip", "identity", "miner");
        if (*error) return ESP_ERR_INVALID_ARG;
        *error = "Choose a paired miner to confirm.";
        *conflict = "Confirm the current pairing after a fresh outlet reading. Use Preview to authorize a new mapping.";
        if (integer("slot", 0, DISPATCH_LIMIT - 1, &slot)) return installation_confirm_mapping(slot, &expected);
        return ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(mode, "enable") || !strcmp(mode, "preview")) {
        *conflict = "Outlet control needs a fresh mapped outlet and valid local time.";
        return installation_control(!strcmp(mode, "enable"));
    }
    if (!strcmp(mode, "resume")) {
        return installation_pause(0, false);
    }
    if (!strcmp(mode, "pause-until")) return installation_pause(0, true);
#ifdef DISPATCH_LINK_TEST
    int link_seconds;
    if (!strcmp(mode, "link-test") && integer("seconds", 5, 45, &link_seconds)) {
        return network_test_link(link_seconds);
    }
#endif
    int minutes;
    if (!strcmp(mode, "pause") && integer("minutes", 1, 1440, &minutes)) {
        return installation_pause(minutes * 60, false);
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t write_scenarios(httpd_req_t *request, const operating_scenario *scenarios)
{
    json_begin(request);
    add(request, "[");
    for (unsigned test = 0; test < OPERATING_SCENARIOS; ++test) {
        if (test) add(request, ",");
        if (!scenarios[test].applicable) {
            add(request, "null");
            continue;
        }
        add(request, "[");
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            if (i) add(request, ",");
            const dispatch_decision decision = scenarios[test].decision[i];
            const dispatch_solar_preset_decision preset = scenarios[test].preset;
            if (preset.slot == i && preset.action != DISPATCH_PRESET_KEEP)
                add(request, "[3,\"Measured lower preset would reduce draw\"]");
            else
                add(request, "[%u,\"%s\",%s]", decision.action, dispatch_explanation(decision.reason),
                    truth(scenarios[test].on_mask & (1u << i)));
        }
        add(request, "]");
    }
    add(request, "]");
    return json_end(request);
}

static esp_err_t update(httpd_req_t *request)
{
    if (!authorize(request)) {
        return reply(request, "401 Unauthorized", "Enter the setup or owner key.");
    }
    const bool importing = !strcmp(request->uri, "/api/import");
    const bool mqtt_config = !strcmp(request->uri, "/api/mqtt");
    const size_t limit = importing ? IMPORT_FORM_BYTES : mqtt_config ? FORM_BYTES : SINGLE_FORM_BYTES;
    char type[64];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/x-www-form-urlencoded") ||
        request->content_len < 1 || request->content_len > limit) {
        return reply(request, "400 Bad Request", "Invalid or oversized form.");
    }
    size_t used = 0;
    const int64_t deadline = esp_timer_get_time() + 3000000;
    while (used < request->content_len && esp_timer_get_time() < deadline) {
        int n = httpd_req_recv(request, form + used, request->content_len - used);
        if (n <= 0) {
            explicit_bzero(form, sizeof(form));
            return ESP_FAIL;
        }
        used += n;
    }
    if (used != request->content_len) {
        explicit_bzero(form, sizeof(form));
        return reply(request, "408 Request Timeout", "Form receipt timed out.");
    }
    form[used] = 0;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    const char *error = "Check the submitted values.";
    const char *conflict = !strcmp(request->uri, "/api/config") ?
        "Reload the current plan, finish any equipment test, and check that a power source is set up for daylight coverage." :
        "Action unavailable in the current mode or clock state.";
    bool wifi = !strcmp(request->uri, "/api/wifi");
    operating_scenario scenarios[OPERATING_SCENARIOS] = {0};
    bool whatif = false;
    if (memchr(form, 0, used)) {
        result = ESP_ERR_INVALID_ARG;
    } else if (wifi && network_setup_active()) {
        result = save_wifi();
    } else if (!wifi && !network_setup_active()) {
        if (importing) {
            result = import_installation(&error);
        } else if (mqtt_config) {
            conflict = "Remove MQTT outlet mappings before changing the broker address, port, or TLS name.";
            result = save_mqtt(&error);
        } else if (!strcmp(request->uri, "/api/config")) {
            char scope[8];
            whatif = field("scope", scope, sizeof(scope)) && !strcmp(scope, "whatif");
            result = save_installation(&error, whatif ? scenarios : NULL);
        } else {
            result = apply_action(&conflict, &error);
        }
    }
    explicit_bzero(form, sizeof(form));
    if (result == ESP_OK) {
        if (whatif) return write_scenarios(request, scenarios);
        return reply(request, "200 OK", wifi ? "Saved. Rejoining the selected network." : "Saved.");
    }
    if (result == ESP_ERR_INVALID_ARG) {
        return reply(request, "400 Bad Request", error);
    }
    if (result == ESP_ERR_INVALID_STATE) {
        return reply(request, "409 Conflict", conflict);
    }
    return reply(request, "503 Service Unavailable", "Could not save the request.");
}

static esp_err_t profile_page(httpd_req_t *request)
{
    if (!authorize(request) || network_setup_active()) return reply(request, "401 Unauthorized", "Enter the owner key.");
    json_begin(request);
    add(request, "{\"version\":%u,\"maxBytes\":%u,\"profiles\":{", INTEGRATION_PROFILE_VERSION, INTEGRATION_PROFILE_BYTES);
    for (profile_family family = 0; family < PROFILE_COUNT; ++family) {
        const profile_status status = profile_store_status(family);
        add(request, "%s\"%s\":{\"custom\":%s,\"savedCustom\":%s,\"pending\":%s,\"invalid\":%s}",
            family ? "," : "", integration_profile_name(family), truth(status.custom), truth(status.saved_custom),
            truth(status.pending), truth(status.invalid));
    }
    const profile_home_assistant *home = integration_profile_get(PROFILE_HOME_ASSISTANT);
    add(request, "},\"homeTemplate\":{\"battery\":\"%s\",\"temperature\":\"%s\",\"actuator\":\"%s\",",
        home->battery, home->temperature, home->actuator);
    add(request, "\"outlet\":\"%s\",\"grid\":\"%s\",\"outletWatts\":\"%s\",\"outletEnergy\":\"%s\"}}",
        home->outlet, home->grid, home->outlet_watts, home->outlet_energy);
    return json_end(request);
}

static esp_err_t profile_update(httpd_req_t *request)
{
    if (!authorize(request) || network_setup_active()) return reply(request, "401 Unauthorized", "Enter the owner key.");
    esp_err_t result;
    if (request->method == HTTP_DELETE) {
        char query[48], family[20];
        if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "family", family, sizeof(family)) != ESP_OK)
            return reply(request, "400 Bad Request", "Select a profile family.");
        result = profile_store_reset(integration_profile_family(family));
    } else {
        char type[32];
        if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type)) != ESP_OK ||
            strcmp(type, "application/json") || request->content_len < 1 ||
            request->content_len > INTEGRATION_PROFILE_BYTES)
            return reply(request, "400 Bad Request", "Choose a DISPATCH JSON profile of at most 1,536 bytes.");
        size_t used = 0;
        const int64_t deadline = esp_timer_get_time() + 3000000;
        while (used < request->content_len && esp_timer_get_time() < deadline) {
            const int n = httpd_req_recv(request, form + used, request->content_len - used);
            if (n <= 0) {
                explicit_bzero(form, sizeof(form));
                return ESP_FAIL;
            }
            used += n;
        }
        if (used != request->content_len) {
            explicit_bzero(form, sizeof(form));
            return reply(request, "408 Request Timeout", "Profile receipt timed out.");
        }
        form[used] = 0;
        result = memchr(form, 0, used) ? ESP_ERR_INVALID_ARG : profile_store_stage(form);
        explicit_bzero(form, sizeof(form));
    }
    if (result == ESP_OK) return reply(request, "200 OK", "Saved. Profile changes apply after a controller restart.");
    if (result == ESP_ERR_INVALID_ARG) return reply(request, "400 Bad Request", "This profile is invalid or incompatible.");
    return reply(request, "503 Service Unavailable", "Could not save the profile.");
}

void web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.max_uri_handlers = 14;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    config.stack_size = 6144;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_page},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_page},
        {.uri = "/api/reports", .method = HTTP_GET, .handler = reports_page},
        {.uri = "/api/timeline", .method = HTTP_GET, .handler = timeline_page},
        {.uri = "/api/import", .method = HTTP_POST, .handler = update},
        {.uri = "/api/timezones", .method = HTTP_GET, .handler = timezones_page},
        {.uri = "/api/discovery", .method = HTTP_GET, .handler = discovery_page},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = update},
        {.uri = "/api/mqtt", .method = HTTP_POST, .handler = update},
        {.uri = "/api/config", .method = HTTP_POST, .handler = update},
        {.uri = "/api/action", .method = HTTP_POST, .handler = update},
        {.uri = "/api/profiles", .method = HTTP_GET, .handler = profile_page},
        {.uri = "/api/profiles", .method = HTTP_POST, .handler = profile_update},
        {.uri = "/api/profiles", .method = HTTP_DELETE, .handler = profile_update},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(*routes); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
}
