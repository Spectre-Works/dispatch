// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "fronius.h"
#include "integration_profile.h"

#include "local_http.h"

#include <math.h>
#include <stdio.h>

static shelly_result decode_power_flow(const json_record *doc, fronius_sample *sample)
{
    const unsigned head = json_field(doc, 1, "Head");
    const unsigned status = json_field(doc, head, "Status");
    if (!json_equals(doc, json_field(doc, status, "Code"), "0")) return SHELLY_PROTOCOL;
    const unsigned data = json_field(doc, json_field(doc, 1, "Body"), "Data");
    const unsigned site = json_field(doc, data, "Site");
    const unsigned mode = json_field(doc, site, "Mode");
    const bool metered = json_equals(doc, mode, "\"meter\"") || json_equals(doc, mode, "\"vague-meter\"") ||
        json_equals(doc, mode, "\"bidirectional\"") || json_equals(doc, mode, "\"ac-coupled\"");
    if (!metered && !json_equals(doc, mode, "\"produce-only\"")) return SHELLY_PROTOCOL;

    const unsigned backup = json_field(doc, site, "BackupMode");
    const unsigned location = json_field(doc, site, "Meter_Location");
    const profile_fronius *mapping = integration_profile_get(PROFILE_FRONIUS);
    double watts;
    // Fronius reports null when the inverter isn't producing or the meter is absent.
    if (json_number(doc, json_field(doc, site, mapping->solar), &watts) && watts >= 0 && watts <= 1000000) {
        sample->solar_known = true;
        sample->solar_watts = (uint32_t)watts;
    }
    if (metered && (!backup || json_equals(doc, backup, "false")) && json_equals(doc, location, "\"grid\"") &&
        json_number(doc, json_field(doc, site, mapping->grid), &watts) && watts >= -1000000 && watts <= 1000000) {
        sample->grid_known = true;
        sample->grid_watts = (int32_t)ceil(watts);
    }
    return SHELLY_OK;
}

shelly_result fronius_power_flow(const char *ip, fronius_sample *sample)
{
    *sample = (fronius_sample){0};
    char header[160];
    const int length = snprintf(header, sizeof(header),
        "GET /solar_api/v1/GetPowerFlowRealtimeData.fcgi HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", ip);
    if (length < 0 || (size_t)length >= sizeof(header)) return SHELLY_PROTOCOL;
    const int code = local_http_response(ip, 80, header, length, NULL, 0, false);
    if (!code) return SHELLY_NETWORK;
    if (code == 401 || code == 403) return SHELLY_AUTH;
    if (code != 200) return SHELLY_PROTOCOL;

    const json_record *doc = local_http_json();
    return doc ? decode_power_flow(doc, sample) : SHELLY_PROTOCOL;
}
