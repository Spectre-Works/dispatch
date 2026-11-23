// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "integration_profile.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static json_record record;

static void compare_sdk_defaults(const char *sdk)
{
    for (profile_family family = 0; family < PROFILE_COUNT; ++family) {
        char path[512], input[INTEGRATION_PROFILE_BYTES + 1];
        assert(snprintf(path, sizeof(path), "%s/profiles/%s.json", sdk, integration_profile_name(family)) < (int)sizeof(path));
        FILE *file = fopen(path, "rb");
        assert(file);
        const size_t n = fread(input, 1, sizeof(input), file);
        fclose(file);
        assert(n && n < sizeof(input));
        input[n] = 0;
        integration_profile profile;
        profile_family loaded;
        assert(integration_profile_decode(input, &record, &loaded, &profile) && loaded == family);
        assert(!integration_profile_digest(loaded, &profile));
    }
}

int main(void)
{
    const char *sdk = getenv("DISPATCH_SDK_PATH");
    if (sdk) compare_sdk_defaults(sdk);
    integration_profile profile;
    profile_family family;
    const char *good = "{\"version\":1,\"family\":\"victron-grid\","
                       "\"fields\":{\"firstRegister\":65530,\"wordOrder\":\"low-first\"}}";
    assert(integration_profile_decode(good, &record, &family, &profile));
    assert(family == PROFILE_VICTRON && profile.victron.first_register == 65530 && profile.victron.low_first);
    const uint64_t changed = integration_profile_digest(family, &profile);
    assert(changed && !integration_profile_revision(family));
    assert(integration_profile_activate(family, &profile));
    assert(integration_profile_revision(family) == changed);
    assert(((const profile_victron *)integration_profile_get(family))->low_first);
    assert(!integration_profile_decode("{\"version\":1,\"family\":\"victron-grid\","
                                       "\"fields\":{\"firstRegister\":65531,\"wordOrder\":\"low-first\"}}",
                                       &record, &family, &profile));
    assert(!integration_profile_decode("{\"version\":1,\"version\":1,\"fields\":{}}",
                                       &record, &family, &profile));
    assert(!integration_profile_decode("{\"version\":2,\"family\":\"tasmota\",\"fields\":{}}",
                                       &record, &family, &profile));
    assert(!integration_profile_decode("{\"version\":1,\"family\":\"tasmota\",\"fields\":"
                                       "{\"statusRelay\":\"auto\",\"watts\":\"Power\",\"kilowattHours\":\"Total\",\"extra\":5}}",
                                       &record, &family, &profile));
    assert(!integration_profile_decode("{\"version\":1,\"family\":\"tasmota\",\"fields\":"
                                       "{\"statusRelay\":\"auto\",\"watts\":\"Power\",\"kilowattHours\":\"Power\"}}",
                                       &record, &family, &profile));
    const char *tasmota = "{\"version\":1,\"family\":\"tasmota\",\"fields\":"
                         "{\"statusRelay\":\"indexed\",\"watts\":\"ActivePower\",\"kilowattHours\":\"Total\"}}";
    assert(integration_profile_decode(tasmota, &record, &family, &profile) && family == PROFILE_TASMOTA);
    assert(profile.tasmota.layout == PROFILE_TASMOTA_INDEXED && !strcmp(profile.tasmota.watts, "ActivePower"));
    char oversized[INTEGRATION_PROFILE_BYTES + 2];
    memset(oversized, ' ', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = 0;
    assert(!integration_profile_decode(oversized, &record, &family, &profile));
    puts("DISPATCH integration profile contract checks passed");
}
