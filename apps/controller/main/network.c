// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "network.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

enum { NETWORK_VERSION = 2, PAIR_SECONDS = 600, BUTTON = GPIO_NUM_41,
       CLOCK_RETRY_SECONDS = 300, CLOCK_REFRESH_SECONDS = 7200, CLOCK_MAX_AGE_SECONDS = 86400 };
_Static_assert(sizeof(network_settings) == 200, "Network settings v1/v2 blob layout");
static const char *const tag = "dispatch";
static const char *const fallback_ntp[] = {"time.cloudflare.com", "time.nist.gov"};
static network_settings saved;
static nvs_handle_t nvs;
static atomic_bool online;
static atomic_uint connection_epoch;
static atomic_uint last_sync;
static atomic_bool time_retry;
static atomic_uint reboot_at;
#ifdef DISPATCH_LINK_TEST
static atomic_uint test_link_at, test_link_off_until;
static bool test_link_down;
#endif
static atomic_bool setup;
static bool pairing_allowed, time_service;
static uint32_t pair_ends;
static char setup_key[SETUP_KEY_LENGTH + 1];
static uint32_t next_connect;
static uint32_t next_clock_retry;
static char setup_input[6 + SETUP_KEY_LENGTH + 1];
static unsigned input_used;
static bool input_overflow;

uint32_t network_uptime(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

bool network_valid_key(const char *value)
{
    return strlen(value) == OWNER_KEY_LENGTH &&
        strspn(value, "0123456789abcdef") == OWNER_KEY_LENGTH;
}

bool network_valid_ntp(const char *value)
{
    const size_t length = strlen(value);
    return length < sizeof(saved.ntp) &&
        strspn(value, "-.0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == length;
}

static bool valid_setup_key(const char *value)
{
    return strlen(value) == SETUP_KEY_LENGTH && strspn(value, "0123456789") == SETUP_KEY_LENGTH;
}

static bool equal_key(const char *value, const char *expected, size_t length)
{
    if (!expected[0] || strlen(value) != length) {
        return false;
    }
    volatile unsigned difference = 0;
    for (unsigned i = 0; i < length; ++i) {
        difference |= (unsigned char)value[i] ^ (unsigned char)expected[i];
    }
    return difference == 0;
}

bool network_authorized(const char *key)
{
    return network_setup_active() ? equal_key(key, setup_key, SETUP_KEY_LENGTH)
                                  : equal_key(key, saved.owner_key, OWNER_KEY_LENGTH);
}

bool network_setup_active(void)
{
    return atomic_load(&setup);
}

bool network_online(void)
{
    return atomic_load(&online);
}

uint32_t network_epoch(void)
{
    return atomic_load(&connection_epoch);
}

const char *network_time_source(void)
{
    return saved.ntp;
}

bool network_time_local_only(void)
{
    return saved.local_time_only;
}

bool network_time_age(uint32_t *seconds)
{
    const uint32_t sync = atomic_load(&last_sync);
    if (!sync) return false;
    *seconds = network_uptime() + 1 - sync;
    return true;
}

time_t network_clock(void)
{
    uint32_t age;
    if (!network_time_age(&age) || age > CLOCK_MAX_AGE_SECONDS) {
        return 0;
    }
    const time_t now = time(NULL);
    return now >= 1735689600 && now < 4102444800LL ? now : 0;
}

static esp_err_t persist_network(const network_settings *settings)
{
    network_settings next = *settings;
    next.version = NETWORK_VERSION;
    esp_err_t result = nvs_set_blob(nvs, "settings", &next, sizeof(next));
    if (result == ESP_OK) {
        result = nvs_commit(nvs);
    }
    explicit_bzero(&next, sizeof(next));
    if (result == ESP_OK) {
        atomic_store(&reboot_at, network_uptime() + 3);
    }
    return result;
}

esp_err_t network_store(const network_settings *settings)
{
    if (!network_setup_active() || !settings->ssid[0] || !network_valid_key(settings->owner_key)) {
        return ESP_ERR_INVALID_STATE;
    }
    return persist_network(settings);
}

esp_err_t network_retry_time(void)
{
    if (!time_service || !network_online() || network_setup_active()) return ESP_ERR_INVALID_STATE;
    atomic_store(&time_retry, true);
    return ESP_OK;
}

esp_err_t network_store_time(const char *server, bool local_only)
{
    if (!network_online() || network_setup_active()) return ESP_ERR_INVALID_STATE;
    if (!network_valid_ntp(server) || (local_only && !server[0])) return ESP_ERR_INVALID_ARG;
    if (!strcmp(server, saved.ntp) && local_only == saved.local_time_only) return network_retry_time();
    network_settings next = saved;
    strcpy(next.ntp, server);
    next.local_time_only = local_only;
    const esp_err_t result = persist_network(&next);
    explicit_bzero(&next, sizeof(next));
    return result;
}

static void synchronized(struct timeval *now)
{
    if (now->tv_sec >= 1735689600 && now->tv_sec < 4102444800LL) {
        atomic_store(&last_sync, network_uptime() + 1);
        ESP_LOGI(tag, "Clock synchronized by NTP, UTC epoch=%lld", (long long)now->tv_sec);
    }
}

static void network_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    if ((base == WIFI_EVENT && event == WIFI_EVENT_STA_DISCONNECTED) ||
        (base == IP_EVENT && event == IP_EVENT_STA_LOST_IP)) {
        if (atomic_exchange(&online, false)) {
            atomic_fetch_add(&connection_epoch, 1);
        }
    } else if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ip = data;
        atomic_store(&online, true);
        ESP_LOGI(tag, "Local web interface at http://" IPSTR "/", IP2STR(&ip->ip_info.ip));
        atomic_store(&time_retry, true);
    }
}

static void wifi_begin(wifi_mode_t mode, wifi_config_t *config)
{
    esp_netif_t *netif = mode == WIFI_MODE_AP
        ? esp_netif_create_default_wifi_ap() : esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(netif ? ESP_OK : ESP_ERR_NO_MEM);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    ESP_ERROR_CHECK(esp_wifi_set_config(mode == WIFI_MODE_AP ? WIFI_IF_AP : WIFI_IF_STA, config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void network_start(void)
{
    ESP_ERROR_CHECK(nvs_open("dispatch_net", NVS_READWRITE, &nvs));
    size_t size = sizeof(saved);
    bool configured = nvs_get_blob(nvs, "settings", &saved, &size) == ESP_OK &&
        size == sizeof(saved) && (saved.version == 1 || saved.version == NETWORK_VERSION) &&
        saved.ssid[0] && saved.owner_key[OWNER_KEY_LENGTH] == 0 &&
        network_valid_key(saved.owner_key);
    if (saved.version == 1) saved.local_time_only = false;
    if (!memchr(saved.ntp, 0, sizeof(saved.ntp)) || !network_valid_ntp(saved.ntp)) {
        saved.ntp[0] = 0;
    }
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    pairing_allowed = !configured || gpio_get_level(BUTTON) == 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, network_event, NULL));

    if (pairing_allowed) {
        usb_serial_jtag_driver_config_t usb = {.tx_buffer_size = 512, .rx_buffer_size = 256};
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
        usb_serial_jtag_vfs_use_driver();
        ESP_LOGI(tag, "Physical setup available over USB");
        return;
    }

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, saved.ssid, strnlen(saved.ssid, sizeof(config.sta.ssid)));
    memcpy(config.sta.password, saved.password, strnlen(saved.password, sizeof(config.sta.password)));
    config.sta.threshold.authmode = saved.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_sntp_config_t sntp = {.sync_cb = synchronized};
    if (saved.ntp[0]) sntp.servers[sntp.num_of_servers++] = saved.ntp;
    if (!saved.local_time_only) {
        for (unsigned i = 0; i < sizeof(fallback_ntp) / sizeof(*fallback_ntp); ++i) {
            if (strcasecmp(saved.ntp, fallback_ntp[i])) sntp.servers[sntp.num_of_servers++] = fallback_ntp[i];
        }
    }
    if (sntp.num_of_servers) {
        ESP_LOGI(tag, "NTP primary %s, %u public fallback servers",
            saved.ntp[0] ? saved.ntp : "automatic", (unsigned)(sntp.num_of_servers - (saved.ntp[0] != 0)));
        time_service = esp_netif_sntp_init(&sntp) == ESP_OK;
        if (!time_service) ESP_LOGW(tag, "Could not initialize NTP");
    }
    wifi_begin(WIFI_MODE_STA, &config);
    explicit_bzero(&config, sizeof(config));
}

static void start_pairing(const char *key)
{
    memcpy(setup_key, key, sizeof(setup_key));
    pair_ends = network_uptime() + PAIR_SECONDS;
    atomic_store(&setup, true);
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    wifi_config_t config = {
        .ap = {.channel = 6, .authmode = WIFI_AUTH_WPA2_PSK, .max_connection = 2},
    };
    snprintf((char *)config.ap.ssid, sizeof(config.ap.ssid), "DISPATCH-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    memcpy(config.ap.password, key, SETUP_KEY_LENGTH);
    wifi_begin(WIFI_MODE_AP, &config);
    ESP_LOGI(tag, "Setup active on %s at http://192.168.4.1/", config.ap.ssid);
    explicit_bzero(&config, sizeof(config));
}

static void read_pairing(void)
{
    char bytes[64];
    int count = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), 0);
    for (int i = 0; i < count; ++i) {
        char ch = bytes[i];
        if (ch == '\r' || ch == '\n') {
            setup_input[input_used] = 0;
            const bool valid = !input_overflow && !strncmp(setup_input, "DPAIR ", 6) &&
                valid_setup_key(setup_input + 6);
            if (valid) {
                start_pairing(setup_input + 6);
            }
            explicit_bzero(setup_input, sizeof(setup_input));
            input_used = 0;
            input_overflow = false;
            if (valid) {
                break;
            }
        } else if (input_used < sizeof(setup_input) - 1) {
            setup_input[input_used++] = ch;
        } else {
            input_overflow = true;
        }
    }
    explicit_bzero(bytes, sizeof(bytes));
}

#ifdef DISPATCH_LINK_TEST
esp_err_t network_test_link(unsigned seconds)
{
    if (!network_online() || network_setup_active() || atomic_load(&test_link_off_until)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t start = network_uptime() + 2;
    atomic_store(&test_link_off_until, start + seconds);
    atomic_store(&test_link_at, start);
    return ESP_OK;
}

static bool defer_connect(uint32_t now)
{
    const uint32_t start = atomic_load(&test_link_at);
    if (!start || now < start) {
        return false;
    }
    const uint32_t until = atomic_load(&test_link_off_until);
    if (!test_link_down) {
        test_link_down = true;
        esp_wifi_disconnect();
    }
    if (now < until) {
        return true;
    }
    test_link_down = false;
    atomic_store(&test_link_at, 0);
    atomic_store(&test_link_off_until, 0);
    next_connect = now;
    return false;
}
#endif

void network_tick(void)
{
    const uint32_t now = network_uptime();
    const uint32_t scheduled = atomic_load(&reboot_at);
    if ((scheduled && now >= scheduled) || (network_setup_active() && now >= pair_ends)) {
        esp_restart();
    }
#ifdef DISPATCH_LINK_TEST
    if (defer_connect(now)) {
        return;
    }
#endif
    if (pairing_allowed) {
        if (!network_setup_active()) {
            read_pairing();
        }
    } else if (!network_online() && now >= next_connect) {
        esp_wifi_connect();
        next_connect = now + 10;
    } else if (time_service && network_online()) {
        uint32_t age;
        if (atomic_exchange(&time_retry, false) ||
            (now >= next_clock_retry && (!network_time_age(&age) || age >= CLOCK_REFRESH_SECONDS))) {
            if (esp_netif_sntp_start() != ESP_OK) ESP_LOGW(tag, "Could not start NTP retry");
            next_clock_retry = now + CLOCK_RETRY_SECONDS;
        }
    }
}
