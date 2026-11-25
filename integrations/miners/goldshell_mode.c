// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "equipment_writes.h"
#include "goldshell_mode.h"
#include "cgminer.h"

#include "json_record.h"
#include "local_http.h"
#include "aes/esp_aes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static json_record doc;
enum { SESSION_TOKEN_BYTES = 640 };
static char write_token[SESSION_TOKEN_BYTES];

typedef struct {
    goldshell_mode_reading reading;
    unsigned default_index, alternate_index, idle_index;
    unsigned selection_offset;
} mode_configuration;

static int request(const char *ip, const char *path, const char *token, const char *payload)
{
    const size_t length = payload ? strlen(payload) : 0;
    // Only the installation worker calls the adapter.
    static char header[SESSION_TOKEN_BYTES + 192];
    int size;
    if (token) {
        size = snprintf(header, sizeof(header),
            "PUT %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
            "Authorization: Bearer %s\r\n%sContent-Length: %u\r\n\r\n",
            path, ip, token, payload ? "Content-Type: application/json\r\n" : "", (unsigned)length);
    } else {
        size = snprintf(header, sizeof(header), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, ip);
    }
    const int result = size > 0 && (size_t)size < sizeof(header)
        ? local_http_response(ip, 80, header, size, payload, length, false) : 0;
    explicit_bzero(header, sizeof(header));
    return result;
}

typedef enum { PLANS_FLAT, PLANS_GROUPED, PLANS_DETECT, PLANS_DETECT_VERIFY_SC } plan_layout;

typedef struct {
    const char *id, *label, *firmware, *schema;
    unsigned default_clock, alternate_clock, boards;
    goldshell_mode alternate;
    plan_layout layout;
    bool bench_verified;
    const char *hardware;
    bool idle, roman_ii;
    unsigned default_ghs, alternate_ghs;
} mode_profile;

static const mode_profile profiles[] = {
    {.id = "Goldshell-HSLITE", .label = "HS Lite", .firmware = "2.2.5", .schema = "v1.2", .default_clock = 800,
     .alternate_clock = 650, .boards = 2, .alternate = GOLDSHELL_MODE_LOW, .layout = PLANS_GROUPED, .bench_verified = true, .idle = true},
    {.id = "Goldshell-HSLITE", .label = "HS Lite", .firmware = "2.2.4", .schema = "v1.1", .default_clock = 800,
     .alternate_clock = 650, .boards = 2, .alternate = GOLDSHELL_MODE_LOW, .layout = PLANS_GROUPED},
    {.id = "Goldshell-HSLITE", .label = "HS Lite", .firmware = "2.2.3", .schema = "v1.1", .default_clock = 800,
     .alternate_clock = 650, .boards = 2, .alternate = GOLDSHELL_MODE_LOW, .layout = PLANS_GROUPED},
    {.id = "Goldshell-SCBoxII", .label = "SC BOX II", .firmware = "2.2.2", .schema = "v1.3",
     .default_clock = 690, .alternate_clock = 525, .boards = 1, .alternate = GOLDSHELL_MODE_LOW, .idle = true, .roman_ii = true},
    {.id = "Goldshell-HSBoxII", .label = "HS BOX II", .firmware = "2.2.6", .schema = "v1.2",
     .default_clock = 850, .alternate_clock = 638, .boards = 1, .alternate = GOLDSHELL_MODE_LOW, .layout = PLANS_GROUPED,
     .idle = true, .roman_ii = true},
    {.id = "Goldshell-KD5Pro", .label = "KD5 Pro", .firmware = "2.2.4", .schema = "v1.2", .default_clock = 825,
     .alternate_clock = 700, .boards = 3, .alternate = GOLDSHELL_MODE_BALANCE, .layout = PLANS_GROUPED, .idle = true},
    {.id = "Goldshell-KD5Pro", .label = "KD5 Pro", .firmware = "2.2.3", .schema = "v1.1", .default_clock = 825,
     .alternate_clock = 700, .boards = 3, .alternate = GOLDSHELL_MODE_BALANCE, .layout = PLANS_GROUPED},
    {.id = "Goldshell-SCLITE", .label = "SC Lite"},
    {.id = "Goldshell-SC5Pro", .label = "SC5 Pro", .firmware = "2.2.0", .default_clock = 840, .boards = 4,
     .alternate = GOLDSHELL_MODE_LOW, .layout = PLANS_DETECT, .hardware = "30.50.SA"},
    {.id = "Goldshell-ARI31", .label = "ARI31", .firmware = "2.2.3", .default_clock = 525, .boards = 4,
     .layout = PLANS_DETECT_VERIFY_SC, .hardware = "30.50.SA"},
    {.id = "Goldshell-SC5PROII", .label = "SC5 Pro II", .layout = PLANS_DETECT,
     .alternate = GOLDSHELL_MODE_LOW, .roman_ii = true, .default_ghs = 14000, .alternate_ghs = 10000},
    {.id = "Goldshell-CAEU04", .label = "CAEU04", .layout = PLANS_DETECT_VERIFY_SC, .default_ghs = 11800, .alternate_ghs = 9500},
    {.id = "Goldshell-CAEU14", .label = "CAEU14", .layout = PLANS_DETECT_VERIFY_SC, .default_ghs = 14000, .alternate_ghs = 10400},
    {.id = "Goldshell-SCUltra", .label = "SC Ultra", .layout = PLANS_DETECT_VERIFY_SC, .default_ghs = 24000, .alternate_ghs = 15000},
    {.id = "Goldshell-SCBox", .label = "SC BOX", .firmware = "2.2.5", .schema = "v2.1", .default_clock = 725, .boards = 1, .idle = true},
    {.id = "Goldshell-HSBox", .label = "HS BOX", .firmware = "2.2.6", .schema = "v2.1", .default_clock = 850, .boards = 1,
     .layout = PLANS_GROUPED, .idle = true},
    {.id = "Goldshell-HS3", .label = "HS3", .firmware = "2.2.7", .schema = "v1.2", .default_clock = 750, .boards = 3,
     .layout = PLANS_DETECT_VERIFY_SC, .idle = true},
    {.id = "Goldshell-HS3SE", .label = "HS3 SE", .firmware = "2.2.4", .schema = "v1.2", .default_clock = 750, .boards = 4,
     .layout = PLANS_DETECT_VERIFY_SC, .idle = true},
    {.id = "Goldshell-HS5", .label = "HS5", .firmware = "2.2.5", .schema = "v1.2", .default_clock = 750, .boards = 4,
     .layout = PLANS_DETECT_VERIFY_SC, .idle = true},
    {.id = "Goldshell-HS6", .label = "HS6", .firmware = "2.2.8", .schema = "v1.2", .default_clock = 715, .boards = 4,
     .layout = PLANS_GROUPED, .idle = true},
    {.id = "Goldshell-HS6SE", .label = "HS6 SE", .firmware = "2.2.7", .schema = "v1.2", .default_clock = 825, .boards = 3,
     .layout = PLANS_GROUPED, .idle = true},
    {.id = "Goldshell-SC6SE", .label = "SC6 SE", .firmware = "2.2.2", .schema = "v1.1", .default_clock = 650, .boards = 4, .idle = true},
    {.id = "Goldshell-KD5", .label = "KD5", .firmware = "2.2.6", .schema = "v1.2", .default_clock = 800, .boards = 4,
     .layout = PLANS_GROUPED, .idle = true},
};

static bool quoted_field(unsigned token, const char *value)
{
    char quoted[48];
    const int length = snprintf(quoted, sizeof(quoted), "\"%s\"", value);
    return length > 0 && (size_t)length < sizeof(quoted) && json_equals(&doc, token, quoted);
}

static bool model_key(unsigned index, char *key, size_t capacity)
{
    if (!index) return false;
    const json_token token = doc.tokens[index];
    static const char vendor[] = "Goldshell";
    const char *name = doc.source + token.from + 1;
    const char *end = doc.source + token.to - 1;
    if (doc.source[token.from] != '"' || end - name <= (int)sizeof(vendor) ||
        strncasecmp(name, vendor, sizeof(vendor) - 1) || (name[sizeof(vendor) - 1] != '-' && name[sizeof(vendor) - 1] != ' ')) return false;
    name += sizeof(vendor);
    size_t used = 0;
    while (name < end) {
        if (*name == '-' || *name == ' ') {
            ++name;
            continue;
        }
        // II appears as UTF-8 or a JSON escape on different versions of Yotta MC.
        if ((end - name == 3 && !memcmp(name, "Ⅱ", 3)) || (end - name == 6 && !memcmp(name, "\\u2161", 6))) {
            if (capacity - used <= 2) return false;
            key[used++] = 'I';
            key[used++] = 'I';
            break;
        }
        if (used + 1 >= capacity || !strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", *name)) return false;
        key[used++] = *name++;
    }
    key[used] = 0;
    return used != 0;
}

static bool model_matches(const char *key, const mode_profile *profile)
{
    const char *id = profile->id + sizeof("Goldshell-") - 1;
    if (!strcasecmp(key, id)) return true;
    if (!profile->roman_ii) return false;
    const size_t prefix = strlen(id) - 2;
    return strlen(key) == prefix + 1 && key[prefix] == '2' && !strncasecmp(key, id, prefix);
}

static void read_identity_field(const char *key, char *out, size_t capacity, const char *allowed)
{
    const unsigned field = json_field(&doc, 1, key);
    if (!field) return;
    const json_token token = doc.tokens[field];
    const char *start = doc.source + token.from;
    const int length = token.to - token.from - 2;
    if (*start == '"' && length > 0 && (size_t)length < capacity &&
        strspn(start + 1, allowed) == (size_t)length) memcpy(out, start + 1, length);
}

static unsigned read_identity(const char *ip, goldshell_mode_reading *reading)
{
    const int status = request(ip, "/mcb/status", NULL, NULL);
    reading->status = status ? GOLDSHELL_MODE_UNSUPPORTED : GOLDSHELL_MODE_OFFLINE;
    if (status != 200 || !json_parse(&doc, local_http_body())) return 0;
    const unsigned model = json_field(&doc, 1, "model");
    if (model) {
        static const char prefix[] = "\"Goldshell-";
        const json_token token = doc.tokens[model];
        const char *start = doc.source + token.from;
        const int length = token.to - token.from - (int)sizeof(prefix);
        if (length > 0 && (size_t)length < sizeof(reading->advertised_model) &&
            !strncasecmp(start, prefix, sizeof(prefix) - 1) &&
            strspn(start + sizeof(prefix) - 1,
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_. ") == (size_t)length) {
            memcpy(reading->advertised_model, start + sizeof(prefix) - 1, length);
        }
    }
    read_identity_field("firmware", reading->firmware, sizeof(reading->firmware), "0123456789.");
    read_identity_field("hardware", reading->hardware, sizeof(reading->hardware),
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._-");
    static const struct { const char *model, *firmware, *label; } restart_profiles[] = {
        {"Goldshell-KDBox", "2.2.4", "KD BOX"},
        {"Goldshell-HSBox", "2.2.6", "HS BOX"},
        {"Goldshell-HSLITE", "2.2.5", "HS Lite"},
    };
    for (unsigned i = 0; i < sizeof(restart_profiles) / sizeof(*restart_profiles); ++i) {
        if (quoted_field(model, restart_profiles[i].model) && !strcmp(reading->firmware, restart_profiles[i].firmware)) {
            reading->model = restart_profiles[i].label;
            reading->restart_supported = true;
            break;
        }
    }
    return model;
}

static const mode_profile *identify(const char *ip, goldshell_mode_reading *reading)
{
    const unsigned model = read_identity(ip, reading);
    char key[32];
    if (!model_key(model, key, sizeof(key))) return NULL;
    for (unsigned i = 0; i < sizeof(profiles) / sizeof(*profiles); ++i) {
        const mode_profile *profile = &profiles[i];
        if (!model_matches(key, profile)) continue;
        reading->model = profile->label;
        if (profile->firmware) {
            if (strcmp(reading->firmware, profile->firmware)) continue;
        } else if (!profile->default_ghs || reading->firmware[0] < '0' || reading->firmware[0] > '9' ||
                   !reading->hardware[0]) continue;
        if (profile->hardware && strcmp(reading->hardware, profile->hardware)) continue;
        reading->experimental = !profile->bench_verified;
        reading->status = GOLDSHELL_MODE_READY;
        return profile;
    }
    return NULL;
}

static int factory_clock(unsigned option)
{
    const unsigned info = json_field(&doc, option, "info");
    if (!info) return -1;
    const json_token token = doc.tokens[info];
    const char *value = doc.source + token.from;
    if (value[0] != '"' || value[1] < '0' || value[1] > '9') return -1;
    char *end;
    const unsigned long clock = strtoul(value + 1, &end, 10);
    return clock <= 10000 && doc.source + token.to - end > 5 && !memcmp(end, " MHz ", 5) ? (int)clock : -1;
}

static bool sc_selected(const char *ip)
{
    if (request(ip, "/mcb/algosetting", NULL, NULL) != 200 || !json_parse(&doc, local_http_body())) return false;
    double selected;
    if (!json_number(&doc, json_field(&doc, 1, "algo_select"), &selected) ||
        selected < 0 || selected > 255 || selected != (unsigned)selected) return false;
    const unsigned entries = json_field(&doc, 1, "algos");
    bool found = false;
    for (unsigned entry = json_first(&doc, entries); entry && entry < doc.tokens[entries].next;
         entry = doc.tokens[entry].next) {
        double id;
        if (!json_number(&doc, json_field(&doc, entry, "id"), &id)) return false;
        if (id != selected) continue;
        if (found || !quoted_field(json_field(&doc, entry, "name"), "blake2b(SC)")) return false;
        found = true;
    }
    return found;
}

static mode_configuration configuration(const char *ip, const mode_profile *profile, goldshell_mode_reading detected)
{
    mode_configuration unavailable = {.reading = detected};
    if (profile->layout == PLANS_DETECT_VERIFY_SC && !sc_selected(ip)) {
        unavailable.reading.status = GOLDSHELL_MODE_ALGORITHM;
        return unavailable;
    }
    const int status = request(ip, "/mcb/setting", NULL, NULL);
    unavailable.reading.status = status ? GOLDSHELL_MODE_UNSUPPORTED : GOLDSHELL_MODE_OFFLINE;
    if (status != 200 || !json_parse(&doc, local_http_body()) ||
        !json_equals(&doc, json_field(&doc, 1, "manual"), "false")) return unavailable;

    const unsigned powerplans = json_field(&doc, 1, "powerplans");
    const unsigned schema = json_field(&doc, 1, "version");
    const unsigned algorithm = json_field(&doc, 1, "algoname");
    const bool by_algorithm = profile->layout == PLANS_GROUPED || (profile->layout >= PLANS_DETECT &&
        json_field(&doc, json_first(&doc, powerplans), "algo"));
    if (profile->schema && !quoted_field(schema, profile->schema)) return unavailable;
    if (!profile->schema && !quoted_field(schema, "v1.0") && !quoted_field(schema, "v1.1") &&
        !quoted_field(schema, "v1.2") && !quoted_field(schema, "v1.3") &&
        !quoted_field(schema, "v2.0") && !quoted_field(schema, "v2.1")) return unavailable;
    if ((by_algorithm || (!profile->schema && algorithm)) &&
        !json_equals(&doc, algorithm, "\"blake2b(SC)\"")) {
        unavailable.reading.status = GOLDSHELL_MODE_ALGORITHM;
        return unavailable;
    }

    unsigned modes = by_algorithm ? 0 : powerplans;
    for (unsigned plan = by_algorithm ? json_first(&doc, powerplans) : 0; plan && plan < doc.tokens[powerplans].next;
         plan = doc.tokens[plan].next) {
        if (!json_equals(&doc, json_field(&doc, plan, "algo"), "\"blake2b(SC)\"")) continue;
        if (modes) return unavailable;
        modes = json_field(&doc, plan, "mode");
        if (!json_first(&doc, modes)) return unavailable;
    }
    mode_configuration config = {.reading = detected};
    bool default_known = false, idle_seen = false;
    unsigned index = 0;
    for (unsigned option = json_first(&doc, modes); option && option < doc.tokens[modes].next;
         option = doc.tokens[option].next, ++index) {
        if (index > 9) return unavailable;
        const unsigned level = json_field(&doc, option, "level");
        if (json_equals(&doc, level, "0")) {
            const int clock = factory_clock(option);
            if (default_known || clock <= 0 || (profile->default_clock && clock != (int)profile->default_clock)) return unavailable;
            default_known = true;
            config.reading.default_clock = clock;
            config.default_index = index;
            continue;
        }
        if (profile->idle && json_equals(&doc, level, "3")) {
            config.reading.idle_supported = !idle_seen && factory_clock(option) == 0;
            idle_seen = true;
            config.idle_index = index;
            continue;
        }
        const goldshell_mode alternate = json_equals(&doc, level, "1") ? GOLDSHELL_MODE_BALANCE :
            json_equals(&doc, level, "2") ? GOLDSHELL_MODE_LOW : GOLDSHELL_MODE_UNKNOWN;
        if (!alternate || (!profile->alternate && profile->schema) ||
            (profile->alternate && alternate != profile->alternate)) continue;
        const int clock = factory_clock(option);
        if (config.reading.alternate || clock <= 0 ||
            (profile->alternate_clock && clock != (int)profile->alternate_clock)) return unavailable;
        config.alternate_index = index;
        config.reading.alternate = alternate;
        config.reading.alternate_clock = clock;
    }
    const unsigned selection = json_field(&doc, 1, "select");
    const bool needs_alternate = profile->alternate || !profile->schema;
    if (!default_known || (needs_alternate ? !config.reading.alternate : !config.reading.idle_supported) ||
        config.reading.alternate_clock >= config.reading.default_clock || !selection ||
        doc.tokens[selection].to - doc.tokens[selection].from != 1) return unavailable;
    config.selection_offset = doc.tokens[selection].from;
    const char current = doc.source[config.selection_offset];
    if (current == (char)('0' + config.default_index)) config.reading.saved = GOLDSHELL_MODE_DEFAULT;
    else if (config.reading.alternate && current == (char)('0' + config.alternate_index)) config.reading.saved = config.reading.alternate;
    else if (config.reading.idle_supported && current == (char)('0' + config.idle_index)) config.reading.saved = GOLDSHELL_MODE_IDLE;
    else return unavailable;
    return config;
}

static bool web_reports_idle(const char *ip)
{
    // Status 6 is the stock dashboard's Idle state. The settings index alone is insufficient.
    return request(ip, "/mcb/cgminer?cgminercmd=devs", NULL, NULL) == 200 &&
        json_parse(&doc, local_http_body()) && json_equals(&doc, json_field(&doc, 1, "status"), "6");
}

static goldshell_mode_reading active_mode(const char *ip, const mode_profile *profile, goldshell_mode_reading reading,
    bool web_idle)
{
    if (reading.status != GOLDSHELL_MODE_READY) return reading;
    if (reading.saved == GOLDSHELL_MODE_IDLE && web_idle) {
        const cgminer_sample sample = cgminer_summary(ip);
        if (!sample.activity_known || !sample.rate_window_seconds || sample.mhs == 0) {
            reading.mode = GOLDSHELL_MODE_IDLE;
            return reading;
        }
    }
    const cgminer_board_sample boards = cgminer_boards(ip, profile->boards);
    reading.board_count = boards.count;
    if (boards.clock == reading.default_clock) reading.mode = GOLDSHELL_MODE_DEFAULT;
    else if (boards.clock && boards.clock == reading.alternate_clock) reading.mode = reading.alternate;
    if (reading.mode == GOLDSHELL_MODE_UNKNOWN) {
        reading.status = GOLDSHELL_MODE_UNCONFIRMED;
    } else if (profile->default_ghs) {
        const double ghs = reading.mode == GOLDSHELL_MODE_DEFAULT ? profile->default_ghs : profile->alternate_ghs;
        // The clock identifies the preset. Recent output checks capacity when the factory board count is unknown.
        if (boards.mhs < ghs * 950 || boards.mhs > ghs * 1050) reading.status = GOLDSHELL_MODE_HASHRATE;
    }
    return reading;
}

static goldshell_mode_reading inspect_preset(const char *ip, const mode_profile *profile, goldshell_mode_reading detected)
{
    const goldshell_mode_reading reading = configuration(ip, profile, detected).reading;
    return active_mode(ip, profile, reading, reading.saved == GOLDSHELL_MODE_IDLE && web_reports_idle(ip));
}

goldshell_mode_reading goldshell_mode_inspect(const char *ip)
{
    goldshell_mode_reading detected = {0};
    const mode_profile *profile = identify(ip, &detected);
    return profile ? inspect_preset(ip, profile, detected) : detected;
}

goldshell_mode_reading goldshell_restart_inspect(const char *ip)
{
    goldshell_mode_reading reading = {0};
    read_identity(ip, &reading);
    if (reading.restart_supported) reading.status = GOLDSHELL_MODE_READY;
    return reading;
}

static goldshell_mode_status login(const char *ip, const char *password, char *token, size_t capacity)
{
    static const unsigned char vendor_key[] = "!!!!!!!!!!!!!!!!";
    const size_t length = strlen(password);
    if (!length || length >= GOLDSHELL_PASSWORD_BYTES) return GOLDSHELL_MODE_AUTH;
    unsigned char block[GOLDSHELL_PASSWORD_BYTES - 1] = {0};
    unsigned char iv[16] = {0};
    memcpy(block, password, length);
    const size_t padded = (length + 15) & ~(size_t)15;
    esp_aes_context aes;
    esp_aes_init(&aes);
    const bool encoded = !esp_aes_setkey(&aes, vendor_key, 128) &&
        !esp_aes_crypt_cbc(&aes, ESP_AES_ENCRYPT, padded, iv, block, block);
    esp_aes_free(&aes);
    char hex[sizeof(block) * 2 + 1] = {0};
    if (encoded) {
        static const char digits[] = "0123456789abcdef";
        for (unsigned i = 0; i < padded; ++i) {
            hex[2 * i] = digits[block[i] >> 4];
            hex[2 * i + 1] = digits[block[i] & 15];
        }
    }
    explicit_bzero(block, sizeof(block));
    char path[160];
    snprintf(path, sizeof(path), "/user/login?username=admin&password=%s&cipher=true", hex);
    explicit_bzero(hex, sizeof(hex));
    const int response = encoded ? request(ip, path, NULL, NULL) : 0;
    explicit_bzero(path, sizeof(path));

    char *body = local_http_body();
    bool valid = false;
    if (response == 200 && json_parse(&doc, body)) {
        const unsigned field = json_field(&doc, 1, "JWT Token");
        if (field) {
            const json_token value = doc.tokens[field];
            const char *first = body + value.from;
            const int bytes = value.to - value.from - 2;
            if (bytes > 0 && (size_t)bytes < capacity && *first == '"' && body[value.to - 1] == '"' &&
                strspn(first + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._") == (size_t)bytes) {
                if (token) {
                    memcpy(token, first + 1, bytes);
                    token[bytes] = 0;
                }
                valid = true;
            }
        }
    }
    explicit_bzero(body, strlen(body));
    return valid ? GOLDSHELL_MODE_READY : !response || response >= 500 ? GOLDSHELL_MODE_OFFLINE : GOLDSHELL_MODE_AUTH;
}

goldshell_mode_reading goldshell_mode_authorize(const char *ip, const char *password,
    const goldshell_mode_reading *expected, bool (*allowed)(void))
{
    goldshell_mode_reading reading = goldshell_mode_inspect(ip);
    if (reading.status != GOLDSHELL_MODE_READY) return reading;
    if (!reading.alternate || !reading.model || !expected->model || strcmp(reading.model, expected->model) ||
        strcmp(reading.firmware, expected->firmware) || strcmp(reading.hardware, expected->hardware) ||
        reading.alternate != expected->alternate || reading.board_count != expected->board_count ||
        reading.default_clock != expected->default_clock || reading.alternate_clock != expected->alternate_clock) {
        reading.status = GOLDSHELL_MODE_CHANGED;
    } else if (!allowed()) {
        reading.status = GOLDSHELL_MODE_DEFERRED;
    } else {
        reading.status = login(ip, password, NULL, SESSION_TOKEN_BYTES);
    }
    return reading;
}

goldshell_mode_reading goldshell_mode_change(const char *ip, const char *password,
    const goldshell_mode_reading *expected, goldshell_mode desired, bool (*allowed)(void))
{
    if (!equipment_writes_allowed()) return (goldshell_mode_reading){.status = GOLDSHELL_MODE_DEFERRED};
    goldshell_mode_reading detected = {0};
    const mode_profile *profile = identify(ip, &detected);
    if (!profile) return detected;
    const goldshell_mode alternate = profile->alternate ? profile->alternate : expected->alternate;
    const bool idle_allowed = profile->idle && expected->idle_supported;
    if (desired != GOLDSHELL_MODE_DEFAULT && (!alternate || desired != alternate) &&
        !(idle_allowed && desired == GOLDSHELL_MODE_IDLE)) {
        detected.status = GOLDSHELL_MODE_UNSUPPORTED;
        return detected;
    }
    if (!expected->model || strcmp(profile->label, expected->model) || strcmp(detected.firmware, expected->firmware) ||
        (expected->hardware[0] && strcmp(detected.hardware, expected->hardware))) {
        detected.status = GOLDSHELL_MODE_CHANGED;
        return detected;
    }
    if (!allowed()) return (goldshell_mode_reading){.status = GOLDSHELL_MODE_DEFERRED};
    const goldshell_mode_status auth = login(ip, password, write_token, sizeof(write_token));
    if (auth != GOLDSHELL_MODE_READY) {
        explicit_bzero(write_token, sizeof(write_token));
        detected.status = auth;
        return detected;
    }

    // Check the web state before fetching the settings snapshot used for a resume write.
    const bool web_idle = expected->mode == GOLDSHELL_MODE_IDLE && web_reports_idle(ip);
    const mode_configuration config = configuration(ip, profile, detected);
    goldshell_mode_reading result = active_mode(ip, profile, config.reading, web_idle);
    if (result.status == GOLDSHELL_MODE_READY && (result.mode != expected->mode || result.alternate != alternate ||
        (desired == GOLDSHELL_MODE_IDLE && !result.idle_supported) ||
        (profile->default_ghs && (result.default_clock != expected->default_clock || result.board_count != expected->board_count)) ||
        (!profile->alternate_clock && result.alternate_clock != expected->alternate_clock))) {
        result.status = GOLDSHELL_MODE_CHANGED;
    } else if (result.status == GOLDSHELL_MODE_READY && result.mode != desired) {
        if (!allowed()) {
            result.status = GOLDSHELL_MODE_DEFERRED;
        } else {
            // The vendor endpoint accepts its complete settings object. Change only the preset index.
            // Send this snapshot before the shared HTTP receive buffer is reused.
            char *body = local_http_body();
            const unsigned selected = desired == GOLDSHELL_MODE_DEFAULT ? config.default_index :
                desired == GOLDSHELL_MODE_IDLE ? config.idle_index : config.alternate_index;
            body[config.selection_offset] = '0' + selected;
            const int write_status = request(ip, "/mcb/setting", write_token, body);
            result = inspect_preset(ip, profile, detected);
            if (write_status == 401 || write_status == 403) result.status = GOLDSHELL_MODE_AUTH;
            else if (write_status && (write_status < 200 || write_status >= 300)) result.status = GOLDSHELL_MODE_UNCONFIRMED;
            else if (result.status != GOLDSHELL_MODE_READY || result.saved != desired || result.mode != desired ||
                     result.alternate != config.reading.alternate ||
                     result.default_clock != config.reading.default_clock ||
                     (profile->default_ghs && result.board_count != expected->board_count) ||
                     result.alternate_clock != config.reading.alternate_clock)
                result.status = GOLDSHELL_MODE_VERIFYING;
        }
    }
    explicit_bzero(write_token, sizeof(write_token));
    return result;
}

goldshell_mode_status goldshell_restart(const char *ip, const char *password,
    const goldshell_mode_reading *expected, bool (*allowed)(void))
{
    if (!equipment_writes_allowed()) return GOLDSHELL_MODE_DEFERRED;
    const goldshell_mode_reading current = goldshell_restart_inspect(ip);
    if (current.status != GOLDSHELL_MODE_READY) return current.status;
    if (!expected->restart_supported || strcmp(current.advertised_model, expected->advertised_model) ||
        strcmp(current.firmware, expected->firmware) || strcmp(current.hardware, expected->hardware))
        return GOLDSHELL_MODE_CHANGED;
    if (!allowed()) return GOLDSHELL_MODE_DEFERRED;

    goldshell_mode_status result = login(ip, password, write_token, sizeof(write_token));
    if (result == GOLDSHELL_MODE_READY) {
        if (!allowed()) {
            result = GOLDSHELL_MODE_DEFERRED;
        } else {
            const int response = request(ip, "/mcb/restart", write_token, NULL);
            if (response >= 200 && response < 300) result = GOLDSHELL_MODE_ACKNOWLEDGED;
            else if (response == 401 || response == 403) result = GOLDSHELL_MODE_AUTH;
            else result = GOLDSHELL_MODE_UNCONFIRMED;
        }
    }
    explicit_bzero(write_token, sizeof(write_token));
    return result;
}
