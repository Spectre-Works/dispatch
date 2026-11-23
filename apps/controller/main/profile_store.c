// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "profile_store.h"

#include "esp_log.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

static nvs_handle_t store;
static bool available;
static uint64_t saved[PROFILE_COUNT];
static bool invalid[PROFILE_COUNT];

static bool decode(const char *text, profile_family *family, integration_profile *profile)
{
    json_record *record = malloc(sizeof(*record));
    if (!record) return false;
    const bool valid = integration_profile_decode(text, record, family, profile);
    free(record);
    return valid;
}

void profile_store_start(void)
{
    const esp_err_t opened = nvs_open("dispatch_sdk", NVS_READWRITE, &store);
    if (opened != ESP_OK) {
        ESP_LOGW("dispatch_sdk", "Profile storage unavailable (%s)", esp_err_to_name(opened));
        return;
    }
    available = true;
    for (profile_family family = 0; family < PROFILE_COUNT; ++family) {
        const char *name = integration_profile_name(family);
        size_t bytes = 0;
        const esp_err_t found = nvs_get_str(store, name, NULL, &bytes);
        if (found == ESP_ERR_NVS_NOT_FOUND) continue;
        char *text = found == ESP_OK && bytes > 1 && bytes <= INTEGRATION_PROFILE_BYTES + 1 ? malloc(bytes) : NULL;
        integration_profile profile;
        profile_family decoded;
        const bool loaded = text && nvs_get_str(store, name, text, &bytes) == ESP_OK &&
            decode(text, &decoded, &profile) && decoded == family &&
            integration_profile_activate(family, &profile);
        free(text);
        if (loaded) {
            saved[family] = integration_profile_revision(family);
        } else {
            invalid[family] = true;
            ESP_LOGW("dispatch_sdk", "Using stock %s mapping after profile load failed", name);
        }
    }
}

profile_status profile_store_status(profile_family family)
{
    const uint64_t active = integration_profile_revision(family);
    return (profile_status){.custom = active != 0, .saved_custom = saved[family] != 0,
                            .pending = active != saved[family], .invalid = invalid[family]};
}

esp_err_t profile_store_stage(char *text)
{
    if (!available) return ESP_ERR_INVALID_STATE;
    integration_profile profile;
    profile_family family;
    if (!decode(text, &family, &profile)) return ESP_ERR_INVALID_ARG;
    // Keep insignificant whitespace out of NVS. Profile strings contain no escapes.
    char *compact = text;
    bool quoted = false;
    for (const char *input = text; *input; ++input) {
        if (*input == '"') quoted = !quoted;
        if (quoted || !strchr(" \t\r\n", *input)) *compact++ = *input;
    }
    *compact = 0;
    esp_err_t result = nvs_set_str(store, integration_profile_name(family), text);
    if (result == ESP_OK) result = nvs_commit(store);
    if (result == ESP_OK) {
        saved[family] = integration_profile_digest(family, &profile);
        invalid[family] = false;
    }
    return result;
}

esp_err_t profile_store_reset(profile_family family)
{
    if (family >= PROFILE_COUNT) return ESP_ERR_INVALID_ARG;
    if (!available) return ESP_ERR_INVALID_STATE;
    esp_err_t result = nvs_erase_key(store, integration_profile_name(family));
    if (result == ESP_OK) result = nvs_commit(store);
    if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) {
        saved[family] = 0;
        invalid[family] = false;
        return ESP_OK;
    }
    return result;
}
