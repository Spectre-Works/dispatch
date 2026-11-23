// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "outlet.h"

#include "tasmota.h"
#include <ctype.h>
#include <string.h>

typedef struct {
    shelly_target target;
    unsigned channel;
} outlet_address;

static bool split(const char *identity, char base[40], unsigned *channel, unsigned default_channel)
{
    const size_t length = strcspn(identity, "@");
    if (!length || length >= 40) return false;
    *channel = default_channel;
    if (identity[length] == '@') {
        const char *digit = identity + length + 1;
        if (!*digit || (*digit == '0' && digit[1])) return false;
        unsigned value = 0;
        for (; *digit; ++digit) {
            if (*digit < '0' || *digit > '9') return false;
            value = 10 * value + (unsigned)(*digit - '0');
            if (value > 199) return false;
        }
        *channel = value;
    }
    memcpy(base, identity, length);
    base[length] = 0;
    return true;
}

static bool tasmota_address(const char *identity, char mac[40], unsigned *channel)
{
    char base[40];
    if (strncmp(identity, "tasmota-", 8) || !split(identity, base, channel, 1) ||
        strlen(base) != 20 || *channel < 1 || *channel > 8) return false;
    if (strspn(base + 8, "0123456789abcdef") != 12) return false;
    for (unsigned i = 0; i < 12; ++i) mac[i] = (char)toupper((unsigned char)base[i + 8]);
    mac[12] = 0;
    return tasmota_valid_identity(mac);
}

static const char *mqtt_address(const char *identity, char mac[40])
{
    if (strncmp(identity, "mqtt-", 5) || strspn(identity + 5, "0123456789abcdef") < 12 ||
        identity[17] != '-' || !mqtt_tasmota_valid_topic(identity + 18)) return NULL;
    for (unsigned i = 0; i < 12; ++i) mac[i] = (char)toupper((unsigned char)identity[i + 5]);
    mac[12] = 0;
    return tasmota_valid_identity(mac) ? identity + 18 : NULL;
}

outlet_kind outlet_identity(const char *identity)
{
    char base[40];
    unsigned channel;
    if (ha_valid_entity(identity, HA_SWITCH)) return OUTLET_HA;
    if (mqtt_address(identity, base)) return OUTLET_MQTT;
    if (tasmota_address(identity, base, &channel)) return OUTLET_TASMOTA;
    if (split(identity, base, &channel, 0) && shelly_valid_identity(base) &&
        shelly_valid_relay_channel(base, channel)) return OUTLET_SHELLY;
    return OUTLET_INVALID;
}

bool outlet_same_target(const shelly_target *a, const shelly_target *b)
{
    const outlet_kind kind_a = outlet_identity(a->identity), kind_b = outlet_identity(b->identity);
    const bool same_ip = !strcmp(a->ip, b->ip);
    if ((kind_a == OUTLET_MQTT && kind_b == OUTLET_TASMOTA) ||
        (kind_b == OUTLET_MQTT && kind_a == OUTLET_TASMOTA)) {
        const shelly_target *mqtt = kind_a == OUTLET_MQTT ? a : b;
        const shelly_target *http = kind_a == OUTLET_TASMOTA ? a : b;
        char mac_a[40], mac_b[40];
        unsigned channel;
        return mqtt_address(mqtt->identity, mac_a) &&
            tasmota_address(http->identity, mac_b, &channel) && channel == 1 && !strcmp(mac_a, mac_b);
    }
    if (kind_a != kind_b) return same_ip && ((kind_a == OUTLET_SHELLY && kind_b == OUTLET_TASMOTA) ||
        (kind_b == OUTLET_SHELLY && kind_a == OUTLET_TASMOTA));
    if (kind_a == OUTLET_MQTT) {
        char mac_a[40], mac_b[40];
        const char *topic_a = mqtt_address(a->identity, mac_a), *topic_b = mqtt_address(b->identity, mac_b);
        return topic_a && topic_b && (!strcmp(mac_a, mac_b) || (same_ip && !strcmp(topic_a, topic_b)));
    }
    if (kind_a == OUTLET_HA) return same_ip && !strcmp(a->identity, b->identity);

    char base_a[40], base_b[40];
    unsigned channel_a, channel_b;
    if (kind_a == OUTLET_TASMOTA) {
        if (!tasmota_address(a->identity, base_a, &channel_a) ||
            !tasmota_address(b->identity, base_b, &channel_b)) return same_ip;
    } else if (!split(a->identity, base_a, &channel_a, 0) ||
               !split(b->identity, base_b, &channel_b, 0)) return same_ip;
    return channel_a == channel_b && (same_ip || !strcmp(base_a, base_b));
}

bool outlet_valid_password(const shelly_target *target)
{
    char base[40];
    unsigned channel;
    return split(target->identity, base, &channel, 0) && shelly_valid_credentials(base, target->password);
}

bool outlet_timed_test(const char *identity)
{
    return outlet_identity(identity) == OUTLET_SHELLY;
}

bool grid_shelly_identity(const char *identity)
{
    char base[40];
    unsigned channel;
    return split(identity, base, &channel, 0) && shelly_valid_identity(base) &&
        shelly_valid_grid_identity(base, channel);
}

shelly_result outlet_status(const shelly_target *target, const ha_endpoint *ha, const mqtt_endpoint *mqtt, shelly_sample *sample)
{
    outlet_address address = {.target = *target};
    shelly_result result = SHELLY_IDENTITY;
    switch (outlet_identity(target->identity)) {
        case OUTLET_HA:
            if (!strcmp(ha->ip, target->ip)) result = ha_switch_status(ha, target->identity, sample);
            break;
        case OUTLET_MQTT: {
            const char *topic = mqtt_address(target->identity, address.target.identity);
            if (topic && mqtt && !strcmp(mqtt->ip, target->ip))
                result = mqtt_tasmota_status(mqtt, address.target.identity, topic, sample);
            break;
        }
        case OUTLET_TASMOTA:
            tasmota_address(target->identity, address.target.identity, &address.channel);
            result = tasmota_status(&address.target, address.channel, sample);
            break;
        case OUTLET_SHELLY:
            split(target->identity, address.target.identity, &address.channel, 0);
            result = shelly_status_channel(&address.target, address.channel, sample);
            break;
        default: break;
    }
    explicit_bzero(&address, sizeof(address));
    return result;
}

shelly_result outlet_set(const shelly_target *target, const ha_endpoint *ha, const mqtt_endpoint *mqtt, bool on, bool (*allowed)(void))
{
    outlet_address address = {.target = *target};
    shelly_result result = SHELLY_IDENTITY;
    switch (outlet_identity(target->identity)) {
        case OUTLET_HA:
            if (!strcmp(ha->ip, target->ip)) result = ha_switch_set(ha, target->identity, on, allowed);
            break;
        case OUTLET_MQTT: {
            const char *topic = mqtt_address(target->identity, address.target.identity);
            if (topic && mqtt && !strcmp(mqtt->ip, target->ip))
                result = mqtt_tasmota_set(mqtt, address.target.identity, topic, on, allowed);
            break;
        }
        case OUTLET_TASMOTA:
            tasmota_address(target->identity, address.target.identity, &address.channel);
            result = tasmota_set(&address.target, address.channel, on, allowed);
            break;
        case OUTLET_SHELLY:
            split(target->identity, address.target.identity, &address.channel, 0);
            result = shelly_set_channel(&address.target, address.channel, on, allowed);
            break;
        default: break;
    }
    explicit_bzero(&address, sizeof(address));
    return result;
}

shelly_result outlet_timed_on(const shelly_target *target, unsigned seconds, bool (*allowed)(void))
{
    if (!outlet_timed_test(target->identity)) return SHELLY_PROTOCOL;
    outlet_address address = {.target = *target};
    split(target->identity, address.target.identity, &address.channel, 0);
    const shelly_result result = shelly_timed_on_channel(&address.target, address.channel, seconds, allowed);
    explicit_bzero(&address, sizeof(address));
    return result;
}

shelly_result grid_shelly_power(const shelly_target *target, int32_t *watts)
{
    if (!grid_shelly_identity(target->identity)) return SHELLY_IDENTITY;
    outlet_address address = {.target = *target};
    split(target->identity, address.target.identity, &address.channel, 0);
    const shelly_result result = shelly_grid_power_channel(&address.target, address.channel, watts);
    explicit_bzero(&address, sizeof(address));
    return result;
}
