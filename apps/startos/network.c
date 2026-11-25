// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "network.h"
#include "discovery.h"
#include "esp_timer.h"
#include "network_update.h"
#include "platform.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

static char owner_key[OWNER_KEY_LENGTH + 1];
static int64_t started;

bool network_valid_key(const char *value)
{
    return strlen(value) == OWNER_KEY_LENGTH && strspn(value, "0123456789abcdef") == OWNER_KEY_LENGTH;
}

void network_start(void)
{
    started = esp_timer_get_time();
    char saved[OWNER_KEY_LENGTH + 1];
    size_t bytes = sizeof(saved);
    const esp_err_t result = preview_file_read("owner.key", saved, &bytes);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        unsigned char random[OWNER_KEY_LENGTH / 2];
        if (RAND_bytes(random, sizeof(random)) != 1) abort();
        for (unsigned i = 0; i < sizeof(random); ++i) snprintf(owner_key + 2 * i, 3, "%02x", random[i]);
        explicit_bzero(random, sizeof(random));
        ESP_ERROR_CHECK(preview_file_write("owner.key", owner_key, OWNER_KEY_LENGTH));
    } else if (result == ESP_OK && bytes == OWNER_KEY_LENGTH) {
        saved[bytes] = 0;
        if (!network_valid_key(saved)) abort();
        memcpy(owner_key, saved, sizeof(owner_key));
    } else {
        fputs("Could not load the DISPATCH owner key\n", stderr);
        abort();
    }
    explicit_bzero(saved, sizeof(saved));
}

bool network_authorized(const char *key)
{
    return network_valid_key(key) && CRYPTO_memcmp(owner_key, key, OWNER_KEY_LENGTH) == 0;
}

bool network_setup_active(void) { return false; }
bool network_online(void) { return true; }
const char *network_hostname(void) { return "dispatch-preview"; }
uint32_t network_epoch(void) { return 1; }

uint32_t network_uptime(void)
{
    return (esp_timer_get_time() - started) / 1000000;
}

time_t network_clock(void)
{
    const time_t now = time(NULL);
    return now >= 1704067200 ? now : 0;
}

bool network_valid_ntp(const char *value) { return !*value; }
bool network_time_age(uint32_t *seconds) { return false; }
bool network_time_local_only(void) { return true; }
const char *network_time_source(void) { return "StartOS system clock"; }
esp_err_t network_retry_time(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t network_store_time(const char *server, bool local_only) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t network_store(const network_settings *settings) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t discovery_request(void) { return ESP_ERR_NOT_SUPPORTED; }
void discovery_read(discovery_view *view) { *view = (discovery_view){0}; }
void discovery_tick(void) {}

void network_update_read(network_update_view *view)
{
    *view = (network_update_view){.installed = DISPATCH_VERSION};
}

esp_err_t network_update_enable(bool enabled) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t network_update_check(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t network_update_seen(const char *version) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t network_update_install(const char *version) { return ESP_ERR_NOT_SUPPORTED; }
bool network_update_staged(void) { return false; }
bool network_update_active(void) { return false; }
void network_update_tick(void) {}
void network_update_restart(bool saved) {}
