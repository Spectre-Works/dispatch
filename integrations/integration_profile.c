// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "integration_profile.h"

#include <stdlib.h>
#include <string.h>

static const profile_shelly_gen1 stock_gen1 = {{
    {"SHPLG-1", "shellyplug-", 1}, {"SHPLG2-1", "shellyplug-", 1},
    {"SHPLG-S", "shellyplug-s-", 1}, {"SHPLG-U1", "shellyplug-u1-", 1},
    {"SHSW-PM", "shelly1pm-", 1}, {"SHSW-25", "shellyswitch25-", 2},
    {"SHEM-3", "shellyem3-", 1},
}};
static const profile_shelly_rpc stock_rpc = {
    .relay_watts = "apower", .energy_object = "aenergy", .relay_wh = "total",
    .single_grid = "act_power", .three_grid = "total_act_power",
};
static const profile_tasmota stock_tasmota = {.watts = "Power", .kwh = "Total"};
static const profile_victron stock_victron = {908, false};
static const profile_fronius stock_fronius = {"P_Grid", "P_PV"};
static const profile_cgminer stock_cgminer = {"MHS 5s", "MHS 20s", "MHS av", "Accepted", "Elapsed"};
static const profile_home_assistant stock_home;

#define STOCK(name) &(name), sizeof(name)
static const struct {
    const char *name;
    const void *stock;
    size_t size;
} families[PROFILE_COUNT] = {
    [PROFILE_SHELLY_GEN1] = {"shelly-gen1", STOCK(stock_gen1)},
    [PROFILE_SHELLY_RPC] = {"shelly-rpc", STOCK(stock_rpc)},
    [PROFILE_TASMOTA] = {"tasmota", STOCK(stock_tasmota)},
    [PROFILE_VICTRON] = {"victron-grid", STOCK(stock_victron)},
    [PROFILE_FRONIUS] = {"fronius", STOCK(stock_fronius)},
    [PROFILE_CGMINER] = {"cgminer", STOCK(stock_cgminer)},
    [PROFILE_HOME_ASSISTANT] = {"home-assistant", STOCK(stock_home)},
};

static void *custom[PROFILE_COUNT];

const char *integration_profile_name(profile_family family)
{
    return family < PROFILE_COUNT ? families[family].name : "";
}

profile_family integration_profile_family(const char *name)
{
    for (profile_family family = 0; family < PROFILE_COUNT; ++family)
        if (!strcmp(name, families[family].name)) return family;
    return PROFILE_COUNT;
}

const void *integration_profile_get(profile_family family)
{
    return custom[family] ? custom[family] : families[family].stock;
}

bool integration_profile_activate(profile_family family, const integration_profile *profile)
{
    if (family >= PROFILE_COUNT) return false;
    const size_t size = families[family].size;
    void *copy = NULL;
    if (memcmp(profile, families[family].stock, size)) {
        copy = malloc(size);
        if (!copy) return false;
        memcpy(copy, profile, size);
    }
    free(custom[family]);
    custom[family] = copy;
    return true;
}

uint64_t integration_profile_digest(profile_family family, const void *mapping)
{
    if (!memcmp(mapping, families[family].stock, families[family].size)) return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char *bytes = mapping;
    for (size_t i = 0; i < families[family].size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}

uint64_t integration_profile_revision(profile_family family)
{
    return integration_profile_digest(family, integration_profile_get(family));
}

static bool read_string(const json_record *record, unsigned token, char *value, size_t capacity,
                        const char *allowed, bool empty)
{
    if (!token) return false;
    const json_token item = record->tokens[token];
    const char *start = record->source + item.from;
    const size_t length = item.to - item.from;
    if (*start != '"' || length < (empty ? 2 : 3) || length - 2 >= capacity ||
        strspn(start + 1, allowed) != length - 2) return false;
    memcpy(value, start + 1, length - 2);
    value[length - 2] = 0;
    return true;
}

static bool integer(const json_record *record, unsigned token, unsigned min, unsigned max, unsigned *value)
{
    if (!token) return false;
    const json_token item = record->tokens[token];
    const char *start = record->source + item.from;
    const size_t length = item.to - item.from;
    if (length > 5 || strspn(start, "0123456789") != length) return false;
    const unsigned result = (unsigned)strtoul(start, NULL, 10);
    if (result < min || result > max) return false;
    *value = result;
    return true;
}

static bool object_size(const json_record *record, unsigned token, unsigned expected)
{
    if (!token || record->source[record->tokens[token].from] != '{') return false;
    const unsigned end = record->tokens[token].next;
    unsigned count = 0;
    for (unsigned key = token + 1; key < end; key = record->tokens[key + 1].next) ++count;
    return count == expected;
}

static bool shelly_models(const json_record *record, unsigned fields, profile_shelly_gen1 *gen1)
{
    enum { STOCK_MODELS = 7 };
    if (!object_size(record, fields, 1)) return false;
    const unsigned models = json_field(record, fields, "models");
    const unsigned first = json_first(record, models);
    if (!first) return false;
    *gen1 = stock_gen1;
    unsigned used = STOCK_MODELS, stock_seen = 0;
    for (unsigned row = first; row < record->tokens[models].next; row = record->tokens[row].next) {
        const unsigned model = json_first(record, row), end = record->tokens[row].next;
        if (!model || model + 3 != end) return false;
        profile_shelly_model entry = {0};
        unsigned relays;
        if (!read_string(record, model, entry.model, sizeof(entry.model), "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-", false) ||
            !read_string(record, model + 1, entry.prefix, sizeof(entry.prefix), "abcdefghijklmnopqrstuvwxyz0123456789-", false) ||
            !integer(record, model + 2, 1, 4, &relays)) return false;
        const size_t length = strlen(entry.prefix);
        if (length < 8 || strncmp(entry.prefix, "shelly", 6) || entry.prefix[length - 1] != '-') return false;
        entry.relays = relays;
        bool stock = false;
        for (unsigned i = 0; i < used; ++i) {
            const profile_shelly_model *known = &gen1->models[i];
            if (strcmp(entry.model, known->model)) continue;
            if (i >= STOCK_MODELS || (stock_seen & (1u << i)) ||
                strcmp(entry.prefix, known->prefix) || entry.relays != known->relays) return false;
            stock_seen |= 1u << i;
            stock = true;
            break;
        }
        if (stock) continue;
        if (used >= PROFILE_SHELLY_MODELS || !strcmp(entry.prefix, "shellyproem-") ||
            !strcmp(entry.prefix, "shellypro3em-")) return false;
        for (unsigned i = 0; i < used; ++i)
            if (!strcmp(entry.prefix, gen1->models[i].prefix)) return false;
        gen1->models[used++] = entry;
    }
    return stock_seen == (1u << STOCK_MODELS) - 1;
}

static bool field_names(const json_record *record, unsigned token, unsigned count, unsigned extra,
                        const char *const keys[], char *const names[], bool distinct)
{
    if (!object_size(record, token, count + extra)) return false;
    for (unsigned i = 0; i < count; ++i) {
        if (!read_string(record, json_field(record, token, keys[i]), names[i], PROFILE_FIELD_BYTES,
                         "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_ -", false)) return false;
        if (distinct)
            for (unsigned j = 0; j < i; ++j)
                if (!strcmp(names[i], names[j])) return false;
    }
    return true;
}

static bool rpc_models(const json_record *record, unsigned models, profile_shelly_rpc *rpc)
{
    if (!models || record->source[record->tokens[models].from] != '[') return false;
    unsigned used = 0;
    for (unsigned row = models + 1; row < record->tokens[models].next; row = record->tokens[row].next) {
        const unsigned prefix = json_first(record, row);
        if (used == PROFILE_RPC_MODELS || !prefix || prefix + 4 != record->tokens[row].next) return false;
        profile_rpc_model *entry = &rpc->models[used];
        unsigned first, last;
        if (!read_string(record, prefix, entry->prefix, sizeof(entry->prefix), "abcdefghijklmnopqrstuvwxyz0123456789-", false) ||
            !integer(record, prefix + 1, 0, 199, &first) || !integer(record, prefix + 2, first, 199, &last)) return false;
        const size_t length = strlen(entry->prefix);
        if (length < 8 || strncmp(entry->prefix, "shelly", 6) || entry->prefix[length - 1] != '-' ||
            !strcmp(entry->prefix, "shellyproem-") || !strcmp(entry->prefix, "shellypro3em-")) return false;
        for (unsigned i = 0; i < 7; ++i)
            if (!strcmp(entry->prefix, stock_gen1.models[i].prefix)) return false;
        for (unsigned i = 0; i < used; ++i)
            if (!strcmp(entry->prefix, rpc->models[i].prefix)) return false;
        entry->first = first;
        entry->last = last;
        const unsigned kind = prefix + 3;
        if (json_equals(record, kind, "\"em\"")) entry->meter = PROFILE_RPC_EM;
        else if (json_equals(record, kind, "\"em1\"")) entry->meter = PROFILE_RPC_EM1;
        else if (!json_equals(record, kind, "\"none\"")) return false;
        ++used;
    }
    return true;
}

static bool entity(const json_record *record, unsigned token, char *output, const char *domain)
{
    if (!read_string(record, token, output, PROFILE_ENTITY_BYTES, "abcdefghijklmnopqrstuvwxyz0123456789_.", true))
        return false;
    if (!*output) return true;
    const char *dot = strchr(output, '.');
    if (!dot || !dot[1] || strlen(dot + 1) > 31 || strchr(dot + 1, '.')) return false;
    if (domain) return (size_t)(dot - output) == strlen(domain) && !strncmp(output, domain, dot - output);
    return !strncmp(output, "number.", 7) || !strncmp(output, "select.", 7) || !strncmp(output, "button.", 7);
}

bool integration_profile_decode(const char *text, json_record *record,
                                profile_family *family, integration_profile *profile)
{
    if (strlen(text) > INTEGRATION_PROFILE_BYTES || !json_parse(record, text) ||
        !object_size(record, 1, 3) || !json_equals(record, json_field(record, 1, "version"), "1")) return false;
    char name[20];
    if (!read_string(record, json_field(record, 1, "family"), name, sizeof(name), "abcdefghijklmnopqrstuvwxyz0123456789-", false))
        return false;
    *family = integration_profile_family(name);
    const unsigned fields = json_field(record, 1, "fields");
    memset(profile, 0, sizeof(*profile));
    switch (*family) {
    case PROFILE_SHELLY_GEN1:
        return shelly_models(record, fields, &profile->gen1);
    case PROFILE_SHELLY_RPC: {
        static const char *const keys[] = {"relayWatts", "relayEnergyObject", "relayWattHours", "singleGridWatts", "threeGridWatts"};
        char *const names[] = {profile->rpc.relay_watts, profile->rpc.energy_object, profile->rpc.relay_wh,
                               profile->rpc.single_grid, profile->rpc.three_grid};
        return field_names(record, fields, 5, 1, keys, names, false) &&
            rpc_models(record, json_field(record, fields, "models"), &profile->rpc);
    }
    case PROFILE_TASMOTA: {
        static const char *const keys[] = {"watts", "kilowattHours"};
        char *const names[] = {profile->tasmota.watts, profile->tasmota.kwh};
        const unsigned layout = json_field(record, fields, "statusRelay");
        if (json_equals(record, layout, "\"indexed\"")) profile->tasmota.layout = PROFILE_TASMOTA_INDEXED;
        else if (json_equals(record, layout, "\"single\"")) profile->tasmota.layout = PROFILE_TASMOTA_SINGLE;
        else if (!json_equals(record, layout, "\"auto\"")) return false;
        return field_names(record, fields, 2, 1, keys, names, true);
    }
    case PROFILE_VICTRON: {
        const unsigned order = json_field(record, fields, "wordOrder");
        unsigned first;
        if (!object_size(record, fields, 2) ||
            !integer(record, json_field(record, fields, "firstRegister"), 0, 65530, &first)) return false;
        profile->victron.first_register = first;
        profile->victron.low_first = json_equals(record, order, "\"low-first\"");
        return profile->victron.low_first || json_equals(record, order, "\"high-first\"");
    }
    case PROFILE_FRONIUS: {
        static const char *const keys[] = {"gridWatts", "solarWatts"};
        char *const names[] = {profile->fronius.grid, profile->fronius.solar};
        return field_names(record, fields, 2, 0, keys, names, true);
    }
    case PROFILE_CGMINER: {
        static const char *const keys[] = {"rate5s", "rate20s", "rateAverage", "accepted", "elapsed"};
        char *const names[] = {profile->cgminer.rate5, profile->cgminer.rate20, profile->cgminer.average,
                               profile->cgminer.accepted, profile->cgminer.elapsed};
        return field_names(record, fields, 5, 0, keys, names, true);
    }
    case PROFILE_HOME_ASSISTANT:
        return object_size(record, fields, 7) &&
            entity(record, json_field(record, fields, "battery"), profile->home.battery, "sensor") &&
            entity(record, json_field(record, fields, "temperature"), profile->home.temperature, "sensor") &&
            entity(record, json_field(record, fields, "actuator"), profile->home.actuator, NULL) &&
            entity(record, json_field(record, fields, "outlet"), profile->home.outlet, "switch") &&
            entity(record, json_field(record, fields, "grid"), profile->home.grid, "sensor") &&
            entity(record, json_field(record, fields, "outletWatts"), profile->home.outlet_watts, "sensor") &&
            entity(record, json_field(record, fields, "outletEnergy"), profile->home.outlet_energy, "sensor");
    case PROFILE_COUNT:
        return false;
    }
    return false;
}
