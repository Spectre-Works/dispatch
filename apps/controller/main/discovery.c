// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "discovery.h"

#include "network.h"
#include "freertos/FreeRTOS.h"
#include "lwip/inet.h"
#include "mdns.h"
#include <string.h>

enum { BROWSE_MS = 1000, RESCAN_SECONDS = 10 };
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static discovery_view scan;
static uint32_t scan_epoch, next_scan;
static bool queued;
// These candidates and progress belong to the installation worker.
static uint32_t candidates[DISCOVERY_LIMIT];
static unsigned candidate_count, candidate_index;
static shelly_target candidate;
static bool identified;

esp_err_t discovery_request(void)
{
    if (!network_online()) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t epoch = network_epoch(), now = network_uptime();
    portENTER_CRITICAL(&mux);
    esp_err_t result = ESP_OK;
    if (scan_epoch != epoch || (!scan.running && now >= next_scan)) {
        scan = (discovery_view){.running = true};
        scan_epoch = epoch;
        queued = true;
    } else if (!scan.running) {
        result = ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&mux);
    return result;
}

void discovery_read(discovery_view *view)
{
    const uint32_t epoch = network_epoch();
    const bool online = network_online();
    portENTER_CRITICAL(&mux);
    *view = online && scan_epoch == epoch ? scan : (discovery_view){.failed = true};
    portEXIT_CRITICAL(&mux);
}

static bool local_address(uint32_t address, const esp_netif_ip_info_t *interface)
{
    const uint32_t host = ntohl(address);
    const uint32_t mask = interface->netmask.addr;
    return (host >> 24 == 10 || host >> 20 == 0xac1 || host >> 16 == 0xc0a8) &&
        mask && ((address ^ interface->ip.addr) & mask) == 0 &&
        address != interface->ip.addr && (address & ~mask) && (address & ~mask) != ~mask;
}

static esp_err_t browse(bool *limited)
{
    esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t interface;
    if (!station || esp_netif_get_ip_info(station, &interface) != ESP_OK || !interface.ip.addr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!network_mdns_ready()) return ESP_ERR_INVALID_STATE;
    mdns_result_t *results = NULL;
    const esp_err_t result = mdns_query_ptr("_shelly", "_tcp", BROWSE_MS, DISCOVERY_LIMIT + 1, &results);
    unsigned responses = 0;
    for (const mdns_result_t *entry = results; entry; entry = entry->next) {
        ++responses;
        if (entry->port != 80 || entry->esp_netif != station || !entry->ttl) {
            continue;
        }
        for (const mdns_ip_addr_t *ip = entry->addr; ip; ip = ip->next) {
            if (ip->addr.type != ESP_IPADDR_TYPE_V4 || !local_address(ip->addr.u_addr.ip4.addr, &interface)) {
                continue;
            }
            const uint32_t address = ip->addr.u_addr.ip4.addr;
            unsigned i = 0;
            while (i < candidate_count && candidates[i] != address) {
                ++i;
            }
            if (i == candidate_count && candidate_count < DISCOVERY_LIMIT) {
                candidates[candidate_count++] = address;
            } else if (i == candidate_count) {
                *limited = true;
            }
        }
    }
    *limited |= responses > DISCOVERY_LIMIT;
    mdns_query_results_free(results);
    return result;
}

void discovery_tick(void)
{
    if (!network_online()) {
        return;
    }
    const uint32_t epoch = network_epoch();
    portENTER_CRITICAL(&mux);
    const bool running = scan.running && scan_epoch == epoch;
    const bool start = running && queued;
    if (start) {
        queued = false;
    }
    portEXIT_CRITICAL(&mux);
    if (!running) {
        return;
    }

    bool failed = false, limited = false, publish = false;
    discovery_device device = {0};
    if (start) {
        candidate_count = candidate_index = 0;
        identified = false;
        failed = browse(&limited) != ESP_OK;
    } else if (!identified) {
        candidate = (shelly_target){0};
        const esp_ip4_addr_t address = {.addr = candidates[candidate_index]};
        esp_ip4addr_ntoa(&address, candidate.ip, sizeof(candidate.ip));
        identified = shelly_identify(&candidate);
        if (!identified) {
            ++candidate_index;
        }
    } else {
        device.result = shelly_status(&candidate, &device.sample);
        device.observed_at = network_uptime();
        strcpy(device.ip, candidate.ip);
        strcpy(device.identity, candidate.identity);
        publish = device.result != SHELLY_IDENTITY;
        identified = false;
        ++candidate_index;
    }

    const bool done = failed || candidate_index == candidate_count;
    const uint32_t now = network_uptime();
    portENTER_CRITICAL(&mux);
    if (scan_epoch == epoch) {
        if (publish) {
            scan.device[scan.count++] = device;
        }
        scan.limited |= limited;
        if (done) {
            scan.running = false;
            scan.failed = failed;
            next_scan = now + RESCAN_SECONDS;
        }
    }
    portEXIT_CRITICAL(&mux);
}
