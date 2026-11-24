// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_NETWORK_H
#define DISPATCH_NETWORK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

enum { OWNER_KEY_LENGTH = 32, SETUP_KEY_LENGTH = 12 };
typedef struct {
    uint32_t version;
    char ssid[33];
    char password[65];
    char owner_key[OWNER_KEY_LENGTH + 1];
    char ntp[64];
    bool local_time_only;
} network_settings;

void network_start(void);
void network_tick(void);
bool network_setup_active(void);
bool network_online(void);
const char *network_hostname(void);
bool network_mdns_ready(void);
uint32_t network_epoch(void);
bool network_authorized(const char *key);
bool network_valid_key(const char *value);
bool network_valid_ntp(const char *value);
esp_err_t network_store(const network_settings *settings);
esp_err_t network_store_time(const char *server, bool local_only);
esp_err_t network_retry_time(void);
const char *network_time_source(void);
bool network_time_local_only(void);
bool network_time_age(uint32_t *seconds);
time_t network_clock(void);
uint32_t network_uptime(void);
#ifdef DISPATCH_LINK_TEST
esp_err_t network_test_link(unsigned seconds);
#endif
#endif
