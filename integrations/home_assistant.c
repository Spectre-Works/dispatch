// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "home_assistant.h"

#include "esp_timer.h"
#include "local_http.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const domains[] = {
    [HA_SWITCH] = "switch", [HA_SENSOR] = "sensor", [HA_NUMBER] = "number",
    [HA_SELECT] = "select", [HA_BUTTON] = "button",
};

bool ha_valid_entity(const char *entity, ha_entity_kind kind)
{
    if ((unsigned)kind >= sizeof(domains) / sizeof(*domains) || strlen(entity) >= HA_ENTITY_BYTES) return false;
    const size_t domain = strlen(domains[kind]);
    if (strncmp(entity, domains[kind], domain) || entity[domain] != '.') return false;
    const char *name = entity + domain + 1;
    return *name && strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_") == strlen(name);
}

bool ha_valid_token(const char *token)
{
    const size_t n = strlen(token);
    return n && n < sizeof(((ha_endpoint *)0)->token) &&
        strspn(token, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == n;
}

static shelly_result request(const ha_endpoint *endpoint, const char *path, const char *body, int64_t deadline)
{
    if (!endpoint->port || !ha_valid_token(endpoint->token)) return SHELLY_AUTH;
    char header[520];
    int length = body
        ? snprintf(header, sizeof(header), "POST %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n"
                   "Authorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n\r\n",
                   path, endpoint->ip, endpoint->port, endpoint->token, (unsigned)strlen(body))
        : snprintf(header, sizeof(header), "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n"
                   "Authorization: Bearer %s\r\n\r\n", path, endpoint->ip, endpoint->port, endpoint->token);
    int code = length > 0 && (size_t)length < sizeof(header)
        ? local_http_response_until(endpoint->ip, endpoint->port, header, length,
                                    body, body ? strlen(body) : 0, false, deadline) : 0;
    explicit_bzero(header, sizeof(header));
    if (!code) return SHELLY_NETWORK;
    if (code == 401 || code == 403 || code == 429) return SHELLY_AUTH;
    if (code == 404) return SHELLY_IDENTITY;
    return code == 200 ? SHELLY_OK : SHELLY_PROTOCOL;
}

static bool bridge_unsigned(const json_record *doc, const char *key, uint32_t max, uint32_t *value)
{
    double number;
    if (!json_number(doc, json_field(doc, 1, key), &number) ||
        number < 0 || number > max || number != (uint32_t)number) return false;
    *value = (uint32_t)number;
    return true;
}

ecu_result sdk_solar_read(const ha_endpoint *endpoint, ecu_reading *reading)
{
    const shelly_result result = request(endpoint, "/api/dispatch/v1/solar", NULL, esp_timer_get_time() + 750000);
    if (result == SHELLY_NETWORK) return ECU_NETWORK;
    if (result != SHELLY_OK) return ECU_PROTOCOL;
    const json_record *doc = local_http_json();
    uint32_t age, inverters, online;
    ecu_reading parsed = {.firmware = "SDK bridge v1"};
    if (!doc || !json_equals(doc, json_field(doc, 1, "version"), "1") ||
        !bridge_unsigned(doc, "ageSeconds", 30, &age) ||
        !bridge_unsigned(doc, "watts", 1000000, &parsed.watts) ||
        !bridge_unsigned(doc, "todayCentiKWh", UINT32_MAX, &parsed.today_centi_kwh) ||
        !bridge_unsigned(doc, "lifetimeDeciKWh", UINT32_MAX, &parsed.lifetime_deci_kwh) ||
        !bridge_unsigned(doc, "inverterCount", UINT16_MAX, &inverters) ||
        !bridge_unsigned(doc, "onlineInverters", inverters, &online)) return ECU_PROTOCOL;
    const unsigned id = json_field(doc, 1, "id");
    if (!id) return ECU_PROTOCOL;
    const json_token token = doc->tokens[id];
    const char *text = doc->source + token.from;
    if (token.to - token.from != 14 || *text != '"' || strspn(text + 1, "0123456789") != 12) return ECU_PROTOCOL;
    memcpy(parsed.id, text + 1, 12);
    parsed.inverters = inverters;
    parsed.online = online;
    *reading = parsed;
    return ECU_OK;
}

static shelly_result read_entity(const ha_endpoint *endpoint, const char *entity, ha_entity_kind kind,
                                 const json_record **doc, int64_t deadline)
{
    if (!ha_valid_entity(entity, kind)) return SHELLY_IDENTITY;
    char path[sizeof("/api/states/") + HA_ENTITY_BYTES];
    snprintf(path, sizeof(path), "/api/states/%s", entity);
    const shelly_result result = request(endpoint, path, NULL, deadline);
    if (result != SHELLY_OK) return result;
    *doc = local_http_json();
    if (!*doc) return SHELLY_PROTOCOL;
    char identity[HA_ENTITY_BYTES + 2];
    snprintf(identity, sizeof(identity), "\"%s\"", entity);
    if (!json_equals(*doc, json_field(*doc, 1, "entity_id"), identity)) return SHELLY_IDENTITY;
    return SHELLY_OK;
}

static shelly_result action(const ha_endpoint *endpoint, ha_entity_kind kind, const char *service,
                             const char *entity, const char *extra, bool (*still_allowed)(void), int64_t deadline)
{
    char path[48], body[HA_ENTITY_BYTES + 2 * HA_OPTION_BYTES + 32];
    snprintf(path, sizeof(path), "/api/services/%s/%s", domains[kind], service);
    const int size = snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"%s}", entity, extra);
    if (size < 0 || (size_t)size >= sizeof(body)) return SHELLY_PROTOCOL;
    if (still_allowed && !still_allowed()) return SHELLY_DEFERRED;
    return request(endpoint, path, body, deadline);
}

static shelly_result read_switch(const ha_endpoint *endpoint, const char *entity, bool *on, int64_t deadline)
{
    const json_record *doc;
    const shelly_result result = read_entity(endpoint, entity, HA_SWITCH, &doc, deadline);
    if (result != SHELLY_OK) return result;
    const unsigned state = json_field(doc, 1, "state");
    *on = json_equals(doc, state, "\"on\"");
    return *on || json_equals(doc, state, "\"off\"") ? SHELLY_OK : SHELLY_PROTOCOL;
}

shelly_result ha_switch_status(const ha_endpoint *endpoint, const char *entity, shelly_sample *sample)
{
    *sample = (shelly_sample){0};
    return read_switch(endpoint, entity, &sample->on, esp_timer_get_time() + 850000);
}

shelly_result ha_switch_set(const ha_endpoint *endpoint, const char *entity, bool on, bool (*still_allowed)(void))
{
    const int64_t deadline = esp_timer_get_time() + 1100000;
    bool observed_on;
    const shelly_result result = read_switch(endpoint, entity, &observed_on, deadline);
    if (result != SHELLY_OK || observed_on == on) return result;
    return action(endpoint, HA_SWITCH, on ? "turn_on" : "turn_off", entity, "", still_allowed, deadline);
}

// Read a bounded HA state or option. The JSON parser already validates framing.
// HA sends UTF-8 labels and escapes quotes and backslashes in those labels.
static bool text_value(const json_record *doc, unsigned field, char *text, size_t capacity)
{
    if (!field) return false;
    const json_token token = doc->tokens[field];
    const char *start = doc->source + token.from, *end = doc->source + token.to - 1;
    if (*start++ != '"') return false;
    size_t used = 0;
    for (; start < end; ++start) {
        char ch = *start;
        if (ch == '\\') {
            ch = *++start;
            if (ch != '"' && ch != '\\' && ch != '/') return false;
        }
        if (used >= capacity - 1) return false;
        text[used++] = ch;
    }
    text[used] = 0;
    return used != 0;
}

static bool number_state(const json_record *doc, unsigned field, double *value)
{
    char number[32], *end;
    if (!text_value(doc, field, number, sizeof(number)) ||
        strspn(number, "+-0123456789.eE") != strlen(number)) return false;
    *value = strtod(number, &end);
    return end != number && !*end && isfinite(*value);
}

static shelly_result read_sensor_until(const ha_endpoint *endpoint, const char *entity, ha_metric metric,
                                       bool cumulative, double *value, int64_t deadline)
{
    const json_record *doc;
    const shelly_result result = read_entity(endpoint, entity, HA_SENSOR, &doc, deadline);
    if (result != SHELLY_OK) return result;
    double number;
    if (!number_state(doc, json_field(doc, 1, "state"), &number)) return SHELLY_PROTOCOL;
    const unsigned attrs = json_field(doc, 1, "attributes");
    const unsigned unit = json_field(doc, attrs, "unit_of_measurement");
    const unsigned class = json_field(doc, attrs, "device_class");
    // An explicit semantic class must agree with the selected measurement role.
    switch (metric) {
    case HA_POWER_W:
        if (class && !json_equals(doc, class, "\"power\"")) return SHELLY_PROTOCOL;
        if (json_equals(doc, unit, "\"kW\"")) number *= 1000;
        else if (json_equals(doc, unit, "\"mW\"")) number /= 1000;
        else if (!json_equals(doc, unit, "\"W\"")) return SHELLY_PROTOCOL;
        if (fabs(number) > 1000000) return SHELLY_PROTOCOL;
        break;
    case HA_ENERGY_WH:
        if (class && !json_equals(doc, class, "\"energy\"")) return SHELLY_PROTOCOL;
        if (cumulative) {
            const unsigned state_class = json_field(doc, attrs, "state_class");
            if (!json_equals(doc, state_class, "\"total\"") &&
                !json_equals(doc, state_class, "\"total_increasing\"")) return SHELLY_PROTOCOL;
        }
        if (json_equals(doc, unit, "\"kWh\"")) number *= 1000;
        else if (json_equals(doc, unit, "\"MWh\"")) number *= 1000000;
        else if (!json_equals(doc, unit, "\"Wh\"")) return SHELLY_PROTOCOL;
        if (number < 0 || number > 1e12) return SHELLY_PROTOCOL;
        break;
    case HA_TEMPERATURE_C:
        if (class && !json_equals(doc, class, "\"temperature\"")) return SHELLY_PROTOCOL;
        if (json_equals(doc, unit, "\"°F\"") || json_equals(doc, unit, "\"\\u00b0F\"")) number = (number - 32) * 5 / 9;
        else if (json_equals(doc, unit, "\"K\"")) number -= 273.15;
        else if (!json_equals(doc, unit, "\"°C\"") && !json_equals(doc, unit, "\"\\u00b0C\"")) return SHELLY_PROTOCOL;
        if (number < -273.15 || number > 1000) return SHELLY_PROTOCOL;
        break;
    case HA_BATTERY_PERCENT:
        if ((class && !json_equals(doc, class, "\"battery\"")) ||
            !json_equals(doc, unit, "\"%\"") || number < 0 || number > 100) return SHELLY_PROTOCOL;
        break;
    default:
        return SHELLY_PROTOCOL;
    }
    *value = number;
    return SHELLY_OK;
}

shelly_result ha_read_sensor(const ha_endpoint *endpoint, const char *entity, ha_metric metric, double *value)
{
    return read_sensor_until(endpoint, entity, metric, false, value, esp_timer_get_time() + 750000);
}

shelly_result ha_mapped_switch_status(const ha_endpoint *endpoint, const char *entity, const char *power_entity,
                                      const char *energy_entity, shelly_sample *sample)
{
    const int64_t deadline = esp_timer_get_time() + 1200000;
    *sample = (shelly_sample){0};
    const shelly_result result = read_switch(endpoint, entity, &sample->on, deadline);
    if (result != SHELLY_OK) return result;
    double value;
    if (power_entity[0] && esp_timer_get_time() < deadline &&
        read_sensor_until(endpoint, power_entity, HA_POWER_W, false, &value, deadline) == SHELLY_OK &&
        value >= 0 && value <= 10000) {
        sample->power_known = true;
        sample->watts = value;
    }
    if (energy_entity[0] && esp_timer_get_time() < deadline &&
        read_sensor_until(endpoint, energy_entity, HA_ENERGY_WH, true, &value, deadline) == SHELLY_OK) {
        sample->energy_known = true;
        sample->total_mwh = (uint64_t)(value * 1000.0 + 0.5);
    }
    return SHELLY_OK;
}

static bool read_options(const json_record *doc, unsigned field, ha_control_state *state)
{
    bool current_listed = false;
    for (unsigned i = json_first(doc, field); i && i < doc->tokens[field].next; i = doc->tokens[i].next) {
        if (state->options == HA_OPTIONS) return false;
        char *option = state->option[state->options++];
        if (!text_value(doc, i, option, HA_OPTION_BYTES)) return false;
        if (!strcmp(option, state->text)) current_listed = true;
    }
    return current_listed;
}

static bool available_number(const json_record *doc, unsigned attributes, unsigned field, ha_control_state *state)
{
    return number_state(doc, field, &state->value) &&
        json_number(doc, json_field(doc, attributes, "min"), &state->min) &&
        json_number(doc, json_field(doc, attributes, "max"), &state->max) &&
        json_number(doc, json_field(doc, attributes, "step"), &state->step) &&
        state->min >= -1e9 && state->max <= 1e9 && state->min < state->max && state->step >= 1e-9 &&
        state->step <= state->max - state->min && state->value >= state->min && state->value <= state->max;
}

static shelly_result control_until(const ha_endpoint *endpoint, const char *entity, ha_entity_kind kind,
                                   ha_control_state *state, int64_t deadline)
{
    const json_record *doc;
    const shelly_result result = read_entity(endpoint, entity, kind, &doc, deadline);
    if (result != SHELLY_OK) return result;
    *state = (ha_control_state){0};
    const unsigned attrs = json_field(doc, 1, "attributes"), field = json_field(doc, 1, "state");
    if (kind == HA_NUMBER) return available_number(doc, attrs, field, state) ? SHELLY_OK : SHELLY_PROTOCOL;
    if (!text_value(doc, field, state->text, sizeof(state->text)) || !strcmp(state->text, "unavailable"))
        return SHELLY_PROTOCOL;
    if (kind == HA_SELECT && strcmp(state->text, "unknown"))
        return read_options(doc, json_field(doc, attrs, "options"), state) ? SHELLY_OK : SHELLY_PROTOCOL;
    if (kind == HA_BUTTON && json_equals(doc, json_field(doc, attrs, "device_class"), "\"restart\""))
        return SHELLY_OK;
    return SHELLY_PROTOCOL;
}

shelly_result ha_control_read(const ha_endpoint *endpoint, const char *entity, ha_entity_kind kind,
                              ha_control_state *state)
{
    if (kind != HA_NUMBER && kind != HA_SELECT && kind != HA_BUTTON) return SHELLY_PROTOCOL;
    return control_until(endpoint, entity, kind, state, esp_timer_get_time() + 850000);
}

shelly_result ha_number_set(const ha_endpoint *endpoint, const char *entity, double value,
                            bool (*still_allowed)(void))
{
    const int64_t deadline = esp_timer_get_time() + 1100000;
    ha_control_state current;
    const shelly_result result = control_until(endpoint, entity, HA_NUMBER, &current, deadline);
    if (result != SHELLY_OK) return result;
    if (!isfinite(value) || value < current.min || value > current.max) return SHELLY_PROTOCOL;
    const double steps = (value - current.min) / current.step;
    if (fabs(steps - round(steps)) > 1e-6) return SHELLY_PROTOCOL;
    if (value == current.value) return SHELLY_OK;
    char extra[40];
    snprintf(extra, sizeof(extra), ",\"value\":%.17g", value);
    return action(endpoint, HA_NUMBER, "set_value", entity, extra, still_allowed, deadline);
}

shelly_result ha_select_set(const ha_endpoint *endpoint, const char *entity, const char *option,
                            bool (*still_allowed)(void))
{
    const int64_t deadline = esp_timer_get_time() + 1100000;
    ha_control_state current;
    const shelly_result result = control_until(endpoint, entity, HA_SELECT, &current, deadline);
    if (result != SHELLY_OK) return result;
    if (!strcmp(current.text, option)) return SHELLY_OK;
    bool available = false;
    for (unsigned i = 0; i < current.options; ++i) {
        if (!strcmp(current.option[i], option)) available = true;
    }
    if (!available) return SHELLY_PROTOCOL;
    char extra[sizeof(",\"option\":\"\"") + 2 * HA_OPTION_BYTES];
    size_t pos = sizeof(",\"option\":\"") - 1;
    memcpy(extra, ",\"option\":\"", pos);
    for (; *option; ++option) {
        if (*option == '"' || *option == '\\') extra[pos++] = '\\';
        extra[pos++] = *option;
    }
    extra[pos++] = '"';
    extra[pos] = 0;
    return action(endpoint, HA_SELECT, "select_option", entity, extra, still_allowed, deadline);
}

shelly_result ha_button_press(const ha_endpoint *endpoint, const char *entity, bool (*still_allowed)(void))
{
    const int64_t deadline = esp_timer_get_time() + 1100000;
    ha_control_state current;
    const shelly_result result = control_until(endpoint, entity, HA_BUTTON, &current, deadline);
    if (result != SHELLY_OK) return result;
    return action(endpoint, HA_BUTTON, "press", entity, "", still_allowed, deadline);
}
