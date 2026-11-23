// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "fronius.h"
#include "integration_profile.h"
#include "local_http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char body[1024];
static int status = 200;
static const char address[] = "192.168.1.72";

const json_record *local_http_json(void)
{
    static json_record doc;
    return json_parse(&doc, body) ? &doc : NULL;
}

int local_http_response(const char *ip, unsigned port, const char *headers, size_t header_bytes,
                        const char *payload, size_t payload_bytes, bool prefix)
{
    assert(!strcmp(ip, address) && port == 80 && header_bytes == strlen(headers));
    assert(!strcmp(headers, "GET /solar_api/v1/GetPowerFlowRealtimeData.fcgi HTTP/1.1\r\n"
        "Host: 192.168.1.72\r\nConnection: close\r\n\r\n"));
    assert(!payload && !payload_bytes && !prefix);
    return status;
}

static void response(unsigned code, const char *site)
{
    snprintf(body, sizeof(body), "{\"Head\":{\"Status\":{\"Code\":%u}},\"Body\":{\"Data\":{\"Site\":%s}}}", code, site);
}

int main(void)
{
    fronius_sample sample;
    response(0, "{\"Mode\":\"bidirectional\",\"Meter_Location\":\"grid\",\"BackupMode\":false,"
        "\"P_Grid\":-109.6,\"P_PV\":930.8,\"P_Load\":-1100,\"P_Akku\":210}");
    assert(fronius_power_flow(address, &sample) == SHELLY_OK);
    assert(sample.solar_known && sample.solar_watts == 930 && sample.grid_known && sample.grid_watts == -109);

    response(0, "{\"Mode\":\"meter\",\"Meter_Location\":\"grid\",\"P_Grid\":0.25,\"P_PV\":null}");
    assert(fronius_power_flow(address, &sample) == SHELLY_OK);
    assert(!sample.solar_known && sample.grid_known && sample.grid_watts == 1);
    response(0, "{\"Mode\":\"produce-only\",\"P_Grid\":null,\"P_PV\":0}");
    assert(fronius_power_flow(address, &sample) == SHELLY_OK);
    assert(sample.solar_known && !sample.solar_watts && !sample.grid_known);
    response(0, "{\"Mode\":\"meter\",\"Meter_Location\":\"load\",\"P_Grid\":710,\"P_PV\":1800}");
    assert(fronius_power_flow(address, &sample) == SHELLY_OK && sample.solar_known && !sample.grid_known);
    response(0, "{\"Mode\":\"bidirectional\",\"Meter_Location\":\"grid\",\"BackupMode\":true,\"P_Grid\":510,\"P_PV\":691}");
    assert(fronius_power_flow(address, &sample) == SHELLY_OK && !sample.grid_known && sample.solar_known);
    response(0, "{\"Mode\":\"meter\",\"Meter_Location\":\"grid\",\"P_Grid\":171000001,\"P_PV\":\"1230\"}");
    assert(fronius_power_flow(address, &sample) == SHELLY_OK && !sample.grid_known && !sample.solar_known);
    response(7, "{\"Mode\":\"meter\",\"Meter_Location\":\"grid\",\"P_Grid\":1010,\"P_PV\":1230}");
    assert(fronius_power_flow(address, &sample) == SHELLY_PROTOCOL && !sample.grid_known && !sample.solar_known);
    status = 302;
    assert(fronius_power_flow(address, &sample) == SHELLY_PROTOCOL);
    status = 0;
    assert(fronius_power_flow(address, &sample) == SHELLY_NETWORK);
    status = 200;
    response(0, "{\"Mode\":\"meter\",\"Meter_Location\":\"grid\",\"P_Grid\":0,\"P_PV\":1}");
    assert(fronius_power_flow(address, &sample) == SHELLY_OK && sample.grid_known && !sample.grid_watts);
    integration_profile custom = {.fronius = {"GridW", "SolarW"}};
    assert(integration_profile_activate(PROFILE_FRONIUS, &custom));
    response(0, "{\"Mode\":\"meter\",\"Meter_Location\":\"grid\",\"GridW\":-720,\"SolarW\":900}");
    assert(fronius_power_flow(address, &sample) == SHELLY_OK &&
        sample.grid_known && sample.grid_watts == -720 && sample.solar_known && sample.solar_watts == 900);
    response(0, "{\"Mode\":\"meter\",\"Meter_Location\":\"grid\",\"BackupMode\":true,\"GridW\":900}");
    assert(fronius_power_flow(address, &sample) == SHELLY_OK && !sample.grid_known);
    puts("Fronius adapter fixture checks passed");
}
