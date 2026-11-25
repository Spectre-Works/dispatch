// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "preset_credentials.h"

#include "dispatch_policy.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    char binding[192];
    char password[GOLDSHELL_PASSWORD_BYTES];
} credential;
static nvs_handle_t store;

static void name_for(unsigned slot, char name[16])
{
    snprintf(name, 16, "preset%u", slot);
}

void preset_credentials_start(nvs_handle_t nvs)
{
    store = nvs;
}

bool preset_credentials_persistent(void)
{
    return true;
}

bool preset_credentials_available(unsigned slot)
{
    if (slot >= DISPATCH_LIMIT) return false;
    char name[16];
    name_for(slot, name);
    size_t bytes = 0;
    return nvs_get_blob(store, name, NULL, &bytes) == ESP_OK && bytes == sizeof(credential);
}

esp_err_t preset_credentials_store(unsigned slot, const char *binding, const char *password)
{
    credential record = {0};
    if (slot >= DISPATCH_LIMIT || strlen(binding) >= sizeof(record.binding) ||
        strlen(password) >= sizeof(record.password)) return ESP_ERR_INVALID_ARG;
    strcpy(record.binding, binding);
    strcpy(record.password, password);
    char name[16];
    name_for(slot, name);
    const esp_err_t result = nvs_set_blob(store, name, &record, sizeof(record));
    explicit_bzero(&record, sizeof(record));
    return result;
}

bool preset_credentials_read(unsigned slot, const char *binding, char password[GOLDSHELL_PASSWORD_BYTES])
{
    if (slot >= DISPATCH_LIMIT) return false;
    char name[16];
    name_for(slot, name);
    credential record;
    size_t bytes = sizeof(record);
    const bool valid = nvs_get_blob(store, name, &record, &bytes) == ESP_OK && bytes == sizeof(record) &&
        memchr(record.binding, 0, sizeof(record.binding)) && strcmp(record.binding, binding) == 0 &&
        record.password[0] && memchr(record.password, 0, sizeof(record.password));
    if (valid) memcpy(password, record.password, sizeof(record.password));
    explicit_bzero(&record, sizeof(record));
    return valid;
}

esp_err_t preset_credentials_forget(unsigned slot)
{
    if (slot >= DISPATCH_LIMIT) return ESP_ERR_INVALID_ARG;
    char name[16];
    name_for(slot, name);
    const esp_err_t result = nvs_erase_key(store, name);
    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}
