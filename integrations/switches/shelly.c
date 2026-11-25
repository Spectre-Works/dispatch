// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "equipment_writes.h"
#include "shelly.h"
#include "integration_profile.h"

#include "shelly_http.h"
#include "local_http.h"
#include "esp_timer.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Network entry points are called serially by the controller worker.
static const json_record *doc;

bool shelly_valid_identity(const char *identity)
{
    static const char prefix[] = "shelly";
    return strlen(identity) > sizeof(prefix) - 1 && !strncmp(identity, prefix, sizeof(prefix) - 1) &&
        strspn(identity, "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(identity);
}

static bool prefix(const char *identity, const char *start)
{
    return !strncmp(identity, start, strlen(start));
}

static const profile_shelly_model *gen1_lookup(const char *identity)
{
    const profile_shelly_gen1 *mapping = integration_profile_get(PROFILE_SHELLY_GEN1);
    for (unsigned i = 0; i < PROFILE_SHELLY_MODELS && mapping->models[i].model[0]; ++i) {
        const profile_shelly_model *profile = &mapping->models[i];
        if (!prefix(identity, profile->prefix)) continue;
        const char *suffix = identity + strlen(profile->prefix);
        const size_t length = strlen(suffix);
        if ((length == 6 || length == 12) && strspn(suffix, "0123456789abcdef") == length) return profile;
    }
    return NULL;
}

static const profile_rpc_model *rpc_model(const char *identity)
{
    const profile_shelly_rpc *mapping = integration_profile_get(PROFILE_SHELLY_RPC);
    for (unsigned i = 0; i < PROFILE_RPC_MODELS && mapping->models[i].prefix[0]; ++i) {
        const profile_rpc_model *model = &mapping->models[i];
        if (!prefix(identity, model->prefix)) continue;
        const char *mac = identity + strlen(model->prefix);
        if (strlen(mac) == 12 && strspn(mac, "0123456789abcdef") == 12) return model;
    }
    return NULL;
}

static profile_rpc_meter rpc_meter(const char *identity)
{
    if (prefix(identity, "shellypro3em-")) return PROFILE_RPC_EM;
    if (prefix(identity, "shellyproem-")) return PROFILE_RPC_EM1;
    const profile_rpc_model *model = rpc_model(identity);
    return model ? model->meter : PROFILE_RPC_NO_METER;
}

bool shelly_valid_relay_channel(const char *identity, unsigned channel)
{
    if (!shelly_valid_identity(identity)) return false;
    const profile_shelly_model *profile = gen1_lookup(identity);
    if (profile) return channel < profile->relays;
    if (prefix(identity, "shellypro3em-")) return channel >= 100 && channel <= 199;
    if (prefix(identity, "shellyproem-")) return channel == 0;
    const profile_rpc_model *model = rpc_model(identity);
    return model ? channel >= model->first && channel <= model->last : channel < 4;
}

bool shelly_valid_grid_identity(const char *identity, unsigned channel)
{
    if (!shelly_valid_identity(identity)) return false;
    const profile_shelly_model *profile = gen1_lookup(identity);
    if (profile) return channel == 0 && !strcmp(profile->model, "SHEM-3");
    const profile_rpc_meter meter = rpc_meter(identity);
    return (channel == 0 && meter == PROFILE_RPC_EM) || (channel < 2 && meter == PROFILE_RPC_EM1);
}

bool shelly_valid_credentials(const char *identity, const char *password)
{
    if (!password[0] || !gen1_lookup(identity)) return true;
    const char *separator = strchr(password, ':');
    return separator && separator != password && separator[1];
}

static bool boolean(unsigned index, bool *value)
{
    if (json_equals(doc, index, "true")) {
        *value = true;
        return true;
    }
    if (json_equals(doc, index, "false")) {
        *value = false;
        return true;
    }
    return false;
}

static shelly_result response_status(int code)
{
    if (!code) return SHELLY_NETWORK;
    if (code == 401 || code == 403 || code == 429) return SHELLY_AUTH;
    doc = code == 200 ? local_http_json() : NULL;
    return doc ? SHELLY_OK : SHELLY_PROTOCOL;
}

static shelly_result gen1_get(const shelly_target *target, const char *path, const char *credentials, int64_t deadline)
{
    return response_status(shelly_get_until(target->ip, path, credentials, local_http_body(), JSON_RECORD_BYTES + 1, deadline));
}

static bool gen1_identity(char identity[40], bool *auth)
{
    const unsigned mac_field = json_field(doc, 1, "mac");
    if (!mac_field || !boolean(json_field(doc, 1, "auth"), auth)) return false;
    const json_token mac_token = doc->tokens[mac_field];
    const char *mac = doc->source + mac_token.from;
    if (mac_token.to - mac_token.from != 14 || mac[0] != '"' || mac[13] != '"' ||
        strspn(mac + 1, "0123456789abcdefABCDEF") != 12) return false;
    const unsigned longid = json_field(doc, 1, "longid");
    if (longid && !json_equals(doc, longid, "0") && !json_equals(doc, longid, "1")) return false;
    const unsigned mac_start = json_equals(doc, longid, "1") ? 0 : 6;
    const unsigned model_field = json_field(doc, 1, "type");
    const profile_shelly_gen1 *mapping = integration_profile_get(PROFILE_SHELLY_GEN1);
    for (unsigned i = 0; i < PROFILE_SHELLY_MODELS && mapping->models[i].model[0]; ++i) {
        const profile_shelly_model *profile = &mapping->models[i];
        char quoted[16];
        snprintf(quoted, sizeof(quoted), "\"%s\"", profile->model);
        if (!json_equals(doc, model_field, quoted)) continue;
        const size_t length = strlen(profile->prefix);
        memcpy(identity, profile->prefix, length);
        for (unsigned j = mac_start; j < 12; ++j) {
            identity[length + j - mac_start] = (char)tolower((unsigned char)mac[j + 1]);
        }
        identity[length + 12 - mac_start] = '\0';
        return true;
    }
    return false;
}

static shelly_result gen1_verify(const shelly_target *target, const char **credentials, int64_t deadline)
{
    const shelly_result status = gen1_get(target, "/shelly", "", deadline);
    if (status != SHELLY_OK) return status;
    char identity[sizeof(target->identity)];
    bool auth;
    if (!gen1_identity(identity, &auth) || strcmp(identity, target->identity)) return SHELLY_IDENTITY;
    if (auth && (!target->password[0] || !shelly_valid_credentials(identity, target->password))) return SHELLY_AUTH;
    *credentials = auth ? target->password : "";
    return SHELLY_OK;
}

static bool read_identity(unsigned object, shelly_target *target)
{
    const unsigned field = json_field(doc, object, "id");
    if (!field) {
        return false;
    }
    json_token token = doc->tokens[field];
    const char *quoted = doc->source + token.from;
    size_t length = token.to - token.from;
    if (length < 3 || length - 2 >= sizeof(target->identity) || quoted[0] != '"' ||
        quoted[length - 1] != '"') {
        return false;
    }
    char identity[sizeof(target->identity)];
    memcpy(identity, quoted + 1, length - 2);
    identity[length - 2] = 0;
    if (!shelly_valid_identity(identity)) return false;
    strcpy(target->identity, identity);
    return true;
}

bool shelly_identify(shelly_target *target)
{
    const int64_t deadline = esp_timer_get_time() + 1500000;
    bool auth;
    if (gen1_get(target, "/shelly", "", deadline) == SHELLY_OK &&
        (read_identity(1, target) || gen1_identity(target->identity, &auth))) return true;
    static const char request[] = "{\"id\":1,\"method\":\"Shelly.GetDeviceInfo\"}";
    char challenge[304];
    const int code = shelly_http_until(target->ip, request, "", local_http_body(), JSON_RECORD_BYTES + 1,
                                       challenge, sizeof(challenge), deadline);
    if (response_status(code) != SHELLY_OK || !json_equals(doc, json_field(doc, 1, "id"), "1") ||
        !read_identity(json_field(doc, 1, "result"), target)) return false;
    char source[sizeof(target->identity) + 2];
    snprintf(source, sizeof(source), "\"%s\"", target->identity);
    return json_equals(doc, json_field(doc, 1, "src"), source);
}

static shelly_result decode(const shelly_target *target, int code, unsigned *result)
{
    const shelly_result status = response_status(code);
    if (status != SHELLY_OK) return status;
    if (!json_equals(doc, json_field(doc, 1, "id"), "1")) return SHELLY_PROTOCOL;
    char quoted_id[sizeof(target->identity) + 2];
    snprintf(quoted_id, sizeof(quoted_id), "\"%s\"", target->identity);
    if (!json_equals(doc, json_field(doc, 1, "src"), quoted_id)) {
        return SHELLY_IDENTITY;
    }
    *result = json_field(doc, 1, "result");
    return *result ? SHELLY_OK : SHELLY_PROTOCOL;
}

static bool channel_is(unsigned result, unsigned channel)
{
    double parsed;
    return json_number(doc, json_field(doc, result, "id"), &parsed) && parsed == channel;
}

static shelly_result rpc(const shelly_target *target, const char *request, const char *preflight, unsigned channel,
                         bool (*still_allowed)(void), unsigned *result, int64_t deadline)
{
    char challenge[304], auth[SHELLY_AUTH_BYTES] = "";
    int code = shelly_http_until(target->ip, preflight ? preflight : request, "",
        local_http_body(), JSON_RECORD_BYTES + 1, challenge, sizeof(challenge), deadline ? deadline : esp_timer_get_time() + 1500000);
    if (code == 401) {
        if (!shelly_digest(target, challenge, auth)) {
            return SHELLY_AUTH;
        }
    } else {
        shelly_result preliminary = decode(target, code, result);
        if (preliminary != SHELLY_OK || !preflight) {
            return preliminary;
        }
        bool observed;
        if (!channel_is(*result, channel) || !boolean(json_field(doc, *result, "output"), &observed)) {
            return SHELLY_PROTOCOL;
        }
    }
    if (still_allowed && !still_allowed()) {
        explicit_bzero(auth, sizeof(auth));
        return SHELLY_DEFERRED;
    }
    code = shelly_http_until(target->ip, request, auth, local_http_body(), JSON_RECORD_BYTES + 1,
        challenge, sizeof(challenge), deadline ? deadline : esp_timer_get_time() + 1500000);
    explicit_bzero(auth, sizeof(auth));
    return decode(target, code, result);
}

static unsigned array_at(const char *key, unsigned index)
{
    const unsigned array = json_field(doc, 1, key);
    unsigned item = json_first(doc, array);
    for (unsigned i = 0; item && i < index; ++i) {
        item = doc->tokens[item].next;
        if (item >= doc->tokens[array].next) return 0;
    }
    return item;
}

static bool gen1_relay(unsigned result, shelly_sample *sample)
{
    if (!boolean(json_field(doc, result, "ison"), &sample->on) ||
        !boolean(json_field(doc, result, "has_timer"), &sample->timer_active)) return false;
    double seconds;
    if (sample->timer_active && json_number(doc, json_field(doc, result, "timer_duration"), &seconds) &&
        seconds > 0 && seconds <= 86400 && floor(seconds) == seconds) {
        sample->timer_seconds = (unsigned)seconds;
        sample->flip_back_timer = true;
    }
    return true;
}

static shelly_result gen1_sample(const shelly_target *target, unsigned channel, const char *credentials,
                                int64_t deadline, shelly_sample *sample)
{
    const shelly_result status = gen1_get(target, "/status", credentials, deadline);
    if (status != SHELLY_OK) return status;
    *sample = (shelly_sample){0};
    if (!gen1_relay(array_at("relays", channel), sample)) return SHELLY_PROTOCOL;
    // CTs on the metering devices are independent of their contactor output.
    const unsigned meter = array_at("meters", channel);
    if (!json_equals(doc, json_field(doc, meter, "is_valid"), "true")) return SHELLY_OK;
    sample->power_known = json_number(doc, json_field(doc, meter, "power"), &sample->watts) &&
        sample->watts >= 0 && sample->watts <= 10000;
    double watt_minutes;
    sample->energy_known = json_number(doc, json_field(doc, meter, "total"), &watt_minutes) &&
        watt_minutes >= 0 && watt_minutes <= 60e12;
    if (sample->energy_known) sample->total_mwh = (uint64_t)(watt_minutes * (1000.0 / 60) + 0.5);
    return SHELLY_OK;
}

static void switch_status_request(char request[80], unsigned channel)
{
    snprintf(request, 80, "{\"id\":1,\"method\":\"Switch.GetStatus\",\"params\":{\"id\":%u}}", channel);
}

shelly_result shelly_status_channel(const shelly_target *target, unsigned channel, shelly_sample *sample)
{
    if (!shelly_valid_relay_channel(target->identity, channel)) return SHELLY_PROTOCOL;
    if (gen1_lookup(target->identity)) {
        const int64_t deadline = esp_timer_get_time() + 1500000;
        const char *credentials;
        const shelly_result status = gen1_verify(target, &credentials, deadline);
        return status == SHELLY_OK ? gen1_sample(target, channel, credentials, deadline, sample) : status;
    }
    char request[80];
    switch_status_request(request, channel);
    unsigned result;
    shelly_result status = rpc(target, request, NULL, channel, NULL, &result, 0);
    if (status != SHELLY_OK) {
        return status;
    }
    *sample = (shelly_sample){0};
    if (!channel_is(result, channel) ||
        !boolean(json_field(doc, result, "output"), &sample->on)) {
        return SHELLY_PROTOCOL;
    }
    const profile_shelly_rpc *mapping = integration_profile_get(PROFILE_SHELLY_RPC);
    sample->power_known = json_number(doc, json_field(doc, result, mapping->relay_watts), &sample->watts) &&
        sample->watts >= 0 && sample->watts <= 10000;
    const unsigned energy = json_field(doc, result, mapping->energy_object);
    double wh;
    sample->energy_known = json_number(doc, json_field(doc, energy, mapping->relay_wh), &wh) &&
        wh >= 0 && wh <= 1e12;
    if (sample->energy_known) {
        sample->total_mwh = (uint64_t)(wh * 1000 + 0.5);
    }
    const unsigned timer = json_field(doc, result, "timer_duration");
    sample->timer_active = timer || json_field(doc, result, "timer_started_at");
    if (sample->timer_active) {
        double seconds;
        const unsigned type = json_field(doc, result, "timer_type");
        if (json_number(doc, timer, &seconds) && seconds > 0 && seconds <= 86400) {
            sample->timer_seconds = (unsigned)seconds;
            sample->flip_back_timer = !type || json_equals(doc, type, "\"toggle_after\"");
        }
    }
    return SHELLY_OK;
}

static bool signed_power(unsigned token, double *power)
{
    return json_number(doc, token, power) && *power >= -1000000 && *power <= 1000000;
}

static shelly_result gen1_grid(const shelly_target *target, int32_t *watts)
{
    const int64_t deadline = esp_timer_get_time() + 750000;
    const char *credentials;
    shelly_result status = gen1_verify(target, &credentials, deadline);
    if (status != SHELLY_OK) return status;
    status = gen1_get(target, "/status", credentials, deadline);
    if (status != SHELLY_OK) return status;
    double total = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned meter = array_at("emeters", i);
        double power;
        if (!json_equals(doc, json_field(doc, meter, "is_valid"), "true") ||
            !signed_power(json_field(doc, meter, "power"), &power)) return SHELLY_PROTOCOL;
        total += power;
    }
    if (total < -1000000 || total > 1000000) return SHELLY_PROTOCOL;
    *watts = (int32_t)ceil(total);
    return SHELLY_OK;
}

static bool errors_clear(unsigned result, const char *key)
{
    const unsigned field = json_field(doc, result, key);
    return !field || (doc->source[doc->tokens[field].from] == '[' &&
        doc->tokens[field].next == field + 1);
}

shelly_result shelly_grid_power_channel(const shelly_target *target, unsigned channel, int32_t *watts)
{
    if (!shelly_valid_grid_identity(target->identity, channel)) return SHELLY_PROTOCOL;
    if (gen1_lookup(target->identity)) return gen1_grid(target, watts);

    const bool single = rpc_meter(target->identity) == PROFILE_RPC_EM1;
    char request[80];
    snprintf(request, sizeof(request), "{\"id\":1,\"method\":\"%s.GetStatus\",\"params\":{\"id\":%u}}",
             single ? "EM1" : "EM", channel);
    static const char *const phases[] = {"a_act_power", "b_act_power", "c_act_power"};
    static const char *const phase_errors[] = {"a_errors", "b_errors", "c_errors"};
    unsigned result;
    const shelly_result status = rpc(target, request, NULL, channel, NULL, &result, esp_timer_get_time() + 750000);
    if (status != SHELLY_OK) return status;
    double power;
    const profile_shelly_rpc *mapping = integration_profile_get(PROFILE_SHELLY_RPC);
    if (!channel_is(result, channel) || !errors_clear(result, "errors") ||
        !signed_power(json_field(doc, result, single ? mapping->single_grid : mapping->three_grid), &power)) {
        return SHELLY_PROTOCOL;
    }
    if (!single) {
        for (unsigned i = 0; i < sizeof(phases) / sizeof(*phases); ++i) {
            double phase;
            if (!errors_clear(result, phase_errors[i]) || !signed_power(json_field(doc, result, phases[i]), &phase)) {
                return SHELLY_PROTOCOL;
            }
        }
    }
    *watts = (int32_t)ceil(power);
    return SHELLY_OK;
}

static bool ready_for_timed_on(const shelly_sample *current)
{
    return !current->on && !current->timer_active && current->power_known && current->energy_known;
}

static shelly_result gen1_set(const shelly_target *target, unsigned channel, bool on, unsigned seconds,
                              bool (*still_allowed)(void))
{
    const int64_t deadline = esp_timer_get_time() + 1500000;
    const char *credentials;
    shelly_result status = gen1_verify(target, &credentials, deadline);
    if (status != SHELLY_OK) return status;
    shelly_sample current;
    status = gen1_sample(target, channel, credentials, deadline, &current);
    if (status != SHELLY_OK) return status;
    if (seconds && !ready_for_timed_on(&current)) return SHELLY_DEFERRED;
    if (still_allowed && !still_allowed()) return SHELLY_DEFERRED;
    char path[64];
    if (seconds) {
        snprintf(path, sizeof(path), "/relay/%u?turn=on&timer=%u", channel, seconds);
    } else {
        snprintf(path, sizeof(path), "/relay/%u?turn=%s", channel, on ? "on" : "off");
    }
    status = gen1_get(target, path, credentials, deadline);
    return status == SHELLY_OK && !gen1_relay(1, &current) ? SHELLY_PROTOCOL : status;
}

static shelly_result rpc_set(const shelly_target *target, unsigned channel, bool on, unsigned seconds,
                             bool (*still_allowed)(void))
{
    char body[120], preflight[80];
    switch_status_request(preflight, channel);
    if (seconds) {
        snprintf(body, sizeof(body), "{\"id\":1,\"method\":\"Switch.Set\",\"params\":"
            "{\"id\":%u,\"on\":true,\"toggle_after\":%u}}", channel, seconds);
    } else {
        snprintf(body, sizeof(body), "{\"id\":1,\"method\":\"Switch.Set\",\"params\":{\"id\":%u,\"on\":%s}}",
                 channel, on ? "true" : "false");
    }
    unsigned result;
    const shelly_result status = rpc(target, body, preflight, channel, still_allowed, &result, 0);
    bool was_on;
    return status == SHELLY_OK && !boolean(json_field(doc, result, "was_on"), &was_on) ? SHELLY_PROTOCOL : status;
}

shelly_result shelly_set_channel(const shelly_target *target, unsigned channel, bool on, bool (*still_allowed)(void))
{
    if (!equipment_writes_allowed()) return SHELLY_DEFERRED;
    if (!shelly_valid_relay_channel(target->identity, channel)) return SHELLY_PROTOCOL;
    return gen1_lookup(target->identity) ? gen1_set(target, channel, on, 0, still_allowed) :
        rpc_set(target, channel, on, 0, still_allowed);
}

shelly_result shelly_timed_on_channel(const shelly_target *target, unsigned channel, unsigned seconds,
                                     bool (*still_allowed)(void))
{
    if (!equipment_writes_allowed()) return SHELLY_DEFERRED;
    if (!seconds || seconds > 240 || !shelly_valid_relay_channel(target->identity, channel)) return SHELLY_PROTOCOL;
    if (gen1_lookup(target->identity)) return gen1_set(target, channel, true, seconds, still_allowed);
    shelly_sample current;
    const shelly_result status = shelly_status_channel(target, channel, &current);
    if (status != SHELLY_OK) return status;
    if (!ready_for_timed_on(&current)) return SHELLY_DEFERRED;
    return rpc_set(target, channel, true, seconds, still_allowed);
}

shelly_result shelly_status(const shelly_target *target, shelly_sample *sample)
{
    return shelly_status_channel(target, 0, sample);
}

shelly_result shelly_grid_power(const shelly_target *target, int32_t *watts)
{
    return shelly_grid_power_channel(target, 0, watts);
}

shelly_result shelly_set(const shelly_target *target, bool on, bool (*still_allowed)(void))
{
    return shelly_set_channel(target, 0, on, still_allowed);
}

shelly_result shelly_timed_on(const shelly_target *target, unsigned seconds, bool (*still_allowed)(void))
{
    return shelly_timed_on_channel(target, 0, seconds, still_allowed);
}
