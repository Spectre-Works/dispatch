// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "equipment_writes.h"
#include "tasmota.h"
#include "integration_profile.h"

#include "esp_timer.h"
#include "local_http.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

bool tasmota_valid_identity(const char *mac)
{
    return strlen(mac) == 12 && strspn(mac, "0123456789ABCDEF") == 12;
}

static shelly_result command(const shelly_target *target, const char *encoded_command,
                             const json_record **doc, int64_t deadline)
{
    char header[384], password[3 * sizeof(target->password)];
    static const char hex[] = "0123456789ABCDEF";
    const size_t size = strnlen(target->password, sizeof(target->password));
    if (size == sizeof(target->password)) return SHELLY_AUTH;
    for (size_t i = 0; i < size; ++i) {
        const unsigned char byte = (unsigned char)target->password[i];
        password[3 * i] = '%';
        password[3 * i + 1] = hex[byte >> 4];
        password[3 * i + 2] = hex[byte & 15];
    }
    password[3 * size] = 0;
    int length = snprintf(header, sizeof(header), "GET /cm?%s%s%scmnd=%s HTTP/1.1\r\n"
        "Host: %s\r\nConnection: close\r\n\r\n", size ? "user=admin&password=" : "", password,
        size ? "&" : "", encoded_command, target->ip);
    explicit_bzero(password, sizeof(password));
    int code = length > 0 && (size_t)length < sizeof(header)
        ? local_http_response_until(target->ip, 80, header, length, NULL, 0, false, deadline) : 0;
    explicit_bzero(header, sizeof(header));
    if (!code) return SHELLY_NETWORK;
    if (code == 401 || code == 403 || code == 429) return SHELLY_AUTH;
    if (code != 200 || !(*doc = local_http_json())) return SHELLY_PROTOCOL;
    // Older WebCommand implementations may report missing credentials with HTTP 200.
    if (json_equals(*doc, json_field(*doc, 1, "WARNING"), "\"Need user=<username>&password=<password>\""))
        return SHELLY_AUTH;
    return SHELLY_OK;
}

static unsigned output_field(const json_record *doc, unsigned object, unsigned channel)
{
    char key[16];
    snprintf(key, sizeof(key), "POWER%u", channel);
    const unsigned indexed = json_field(doc, object, key);
    const unsigned unindexed = channel == 1 ? json_field(doc, object, "POWER") : 0;
    return indexed && unindexed ? 0 : indexed ? indexed : unindexed;
}

static bool output_state(const json_record *doc, unsigned power, bool *on)
{
    if (json_equals(doc, power, "\"ON\"")) {
        *on = true;
        return true;
    }
    if (json_equals(doc, power, "\"OFF\"")) {
        *on = false;
        return true;
    }
    return false;
}

bool tasmota_output(const json_record *doc, unsigned state, unsigned channel, bool *on)
{
    return output_state(doc, output_field(doc, state, channel), on);
}

bool tasmota_observed_output(const json_record *doc, unsigned state, unsigned channel, bool *on)
{
    const profile_tasmota *mapping = integration_profile_get(PROFILE_TASMOTA);
    if (mapping->layout == PROFILE_TASMOTA_AUTO) return tasmota_output(doc, state, channel, on);
    if (mapping->layout == PROFILE_TASMOTA_SINGLE)
        return channel == 1 && tasmota_only_output(doc, state) &&
            output_state(doc, json_field(doc, state, "POWER"), on);
    char key[16];
    snprintf(key, sizeof(key), "POWER%u", channel);
    return (channel != 1 || !json_field(doc, state, "POWER")) &&
        output_state(doc, json_field(doc, state, key), on);
}

bool tasmota_matches(const json_record *doc, const char *identity)
{
    const unsigned network = json_field(doc, 1, "StatusNET");
    const unsigned field = json_field(doc, network, "Mac");
    if (!field) return false;
    char expected[20];
    snprintf(expected, sizeof(expected), "\"%.2s:%.2s:%.2s:%.2s:%.2s:%.2s\"",
        identity, identity + 2, identity + 4, identity + 6, identity + 8, identity + 10);
    json_token token = doc->tokens[field];
    return token.to - token.from == 19 && !strncasecmp(doc->source + token.from, expected, 19);
}

bool tasmota_only_output(const json_record *doc, unsigned state)
{
    // Scalar counters in StatusSNS describe the one monitored load. Multiple
    // outputs can expose shared or array telemetry with different semantics.
    for (unsigned extra = 2; extra <= 32; ++extra) {
        if (output_field(doc, state, extra)) return false;
    }
    return true;
}

void tasmota_meter(const json_record *doc, shelly_sample *sample)
{
    const unsigned sensors = json_field(doc, 1, "StatusSNS");
    const unsigned energy = json_field(doc, sensors, "ENERGY");
    const profile_tasmota *mapping = integration_profile_get(PROFILE_TASMOTA);
    double total;
    sample->power_known = json_number(doc, json_field(doc, energy, mapping->watts), &sample->watts) &&
        sample->watts >= 0 && sample->watts <= 10000;
    sample->energy_known = json_number(doc, json_field(doc, energy, mapping->kwh), &total) &&
        total >= 0 && total <= 1e9;
    if (sample->energy_known) sample->total_mwh = (uint64_t)(total * 1000000.0 + 0.5);
}

static shelly_result status_until(const shelly_target *target, unsigned channel, shelly_sample *sample,
                                  int64_t deadline)
{
    if (channel < 1 || channel > 8 || !tasmota_valid_identity(target->identity)) return SHELLY_IDENTITY;
    const json_record *doc;
    const shelly_result result = command(target, "Status%200", &doc, deadline);
    if (result != SHELLY_OK) return result;
    if (!tasmota_matches(doc, target->identity)) return SHELLY_IDENTITY;
    const unsigned state = json_field(doc, 1, "StatusSTS");
    *sample = (shelly_sample){0};
    if (!tasmota_observed_output(doc, state, channel, &sample->on)) return SHELLY_PROTOCOL;
    if (channel == 1 && tasmota_only_output(doc, state)) tasmota_meter(doc, sample);
    return SHELLY_OK;
}

shelly_result tasmota_status(const shelly_target *target, unsigned channel, shelly_sample *sample)
{
    return status_until(target, channel, sample, esp_timer_get_time() + 850000);
}

shelly_result tasmota_set(const shelly_target *target, unsigned channel, bool on, bool (*still_allowed)(void))
{
    if (!equipment_writes_allowed()) return SHELLY_DEFERRED;
    const int64_t deadline = esp_timer_get_time() + 1100000;
    shelly_sample current;
    const shelly_result preflight = status_until(target, channel, &current, deadline);
    if (preflight != SHELLY_OK || current.on == on) return preflight;
    if (still_allowed && !still_allowed()) return SHELLY_DEFERRED;
    char request[24];
    snprintf(request, sizeof(request), "Power%u%%20%s", channel, on ? "On" : "Off");
    const json_record *doc;
    const shelly_result result = command(target, request, &doc, deadline);
    if (result != SHELLY_OK) return result;
    bool replied_on;
    return tasmota_output(doc, 1, channel, &replied_on) && replied_on == on ? SHELLY_OK : SHELLY_PROTOCOL;
}
