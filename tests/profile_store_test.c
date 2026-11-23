// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "profile_store.h"
#include "nvs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char saved[PROFILE_COUNT][INTEGRATION_PROFILE_BYTES + 1];
static esp_err_t write_result;

esp_err_t nvs_open(const char *name, int flags, nvs_handle_t *handle)
{
    assert(!strcmp(name, "dispatch_sdk") && flags == NVS_READWRITE);
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *text, size_t *bytes)
{
    assert(handle == 1);
    const profile_family family = integration_profile_family(key);
    assert(family < PROFILE_COUNT);
    const char *value = saved[family];
    if (!*value) return ESP_ERR_NVS_NOT_FOUND;
    const size_t needed = strlen(value) + 1;
    if (text) {
        assert(*bytes >= needed);
        memcpy(text, value, needed);
    }
    *bytes = needed;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *text)
{
    assert(handle == 1);
    const profile_family family = integration_profile_family(key);
    assert(family < PROFILE_COUNT && strlen(text) < sizeof(saved[family]));
    if (write_result == ESP_OK) strcpy(saved[family], text);
    return write_result;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(handle == 1);
    const profile_family family = integration_profile_family(key);
    assert(family < PROFILE_COUNT);
    if (!saved[family][0]) return ESP_ERR_NVS_NOT_FOUND;
    saved[family][0] = 0;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    return ESP_OK;
}

int main(void)
{
    profile_store_start();
    char profile[] =
        "{ \"version\": 1, \"family\": \"fronius\", \"fields\": { \"gridWatts\": \"Grid W\", \"solarWatts\": \"SolarW\" } }";
    assert(profile_store_stage(profile) == ESP_OK);
    assert(!profile_store_status(PROFILE_FRONIUS).custom && profile_store_status(PROFILE_FRONIUS).pending);
    assert(!strcmp(saved[PROFILE_FRONIUS],
        "{\"version\":1,\"family\":\"fronius\",\"fields\":{\"gridWatts\":\"Grid W\",\"solarWatts\":\"SolarW\"}}"));
    char bad[] = "{\"version\":2,\"family\":\"fronius\",\"fields\":{}}";
    assert(profile_store_stage(bad) == ESP_ERR_INVALID_ARG);
    assert(!strcmp(saved[PROFILE_FRONIUS], profile));
    char replacement[] =
        "{\"version\":1,\"family\":\"fronius\",\"fields\":{\"gridWatts\":\"Other\",\"solarWatts\":\"SolarW\"}}";
    write_result = ESP_FAIL;
    assert(profile_store_stage(replacement) == ESP_FAIL);
    assert(!strcmp(saved[PROFILE_FRONIUS], profile));

    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        strcpy(saved[PROFILE_TASMOTA], "{\"version\":99}");
        profile_store_start();
        assert(profile_store_status(PROFILE_FRONIUS).custom && !profile_store_status(PROFILE_FRONIUS).pending);
        const profile_fronius *active = integration_profile_get(PROFILE_FRONIUS);
        assert(!strcmp(active->grid, "Grid W"));
        assert(profile_store_status(PROFILE_TASMOTA).invalid && !profile_store_status(PROFILE_TASMOTA).custom);
        assert(profile_store_reset(PROFILE_FRONIUS) == ESP_OK);
        assert(profile_store_status(PROFILE_FRONIUS).pending && !profile_store_status(PROFILE_FRONIUS).saved_custom);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(profile_store_reset(PROFILE_FRONIUS) == ESP_OK);
    assert(!profile_store_status(PROFILE_FRONIUS).pending && !saved[PROFILE_FRONIUS][0]);
    profile_store_start();
    assert(!profile_store_status(PROFILE_FRONIUS).custom);
    puts("DISPATCH profile upload, startup activation, and stock restoration checks passed");
}
