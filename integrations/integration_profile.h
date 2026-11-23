// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_INTEGRATION_PROFILE_H
#define DISPATCH_INTEGRATION_PROFILE_H

#include "json_record.h"
#include <stddef.h>

enum { INTEGRATION_PROFILE_VERSION = 1, INTEGRATION_PROFILE_BYTES = 1536,
       PROFILE_FIELD_BYTES = 24, PROFILE_ENTITY_BYTES = 40, PROFILE_SHELLY_MODELS = 11,
       PROFILE_RPC_MODELS = 4 };

typedef enum {
    PROFILE_SHELLY_GEN1, PROFILE_SHELLY_RPC, PROFILE_TASMOTA, PROFILE_VICTRON,
    PROFILE_FRONIUS, PROFILE_CGMINER, PROFILE_HOME_ASSISTANT, PROFILE_COUNT
} profile_family;

typedef struct {
    char model[12], prefix[19];
    uint8_t relays;
} profile_shelly_model;

typedef struct {
    profile_shelly_model models[PROFILE_SHELLY_MODELS];
} profile_shelly_gen1;

typedef enum { PROFILE_RPC_NO_METER, PROFILE_RPC_EM, PROFILE_RPC_EM1 } profile_rpc_meter;

typedef struct {
    char prefix[24];
    uint8_t first, last, meter;
} profile_rpc_model;

typedef struct {
    char relay_watts[PROFILE_FIELD_BYTES], energy_object[PROFILE_FIELD_BYTES];
    char relay_wh[PROFILE_FIELD_BYTES], single_grid[PROFILE_FIELD_BYTES], three_grid[PROFILE_FIELD_BYTES];
    profile_rpc_model models[PROFILE_RPC_MODELS];
} profile_shelly_rpc;

typedef enum { PROFILE_TASMOTA_AUTO, PROFILE_TASMOTA_INDEXED, PROFILE_TASMOTA_SINGLE } profile_tasmota_layout;
typedef struct {
    char watts[PROFILE_FIELD_BYTES], kwh[PROFILE_FIELD_BYTES];
    uint8_t layout;
} profile_tasmota;
typedef struct { uint16_t first_register; bool low_first; } profile_victron;
typedef struct { char grid[PROFILE_FIELD_BYTES], solar[PROFILE_FIELD_BYTES]; } profile_fronius;

typedef struct {
    char rate5[PROFILE_FIELD_BYTES], rate20[PROFILE_FIELD_BYTES], average[PROFILE_FIELD_BYTES];
    char accepted[PROFILE_FIELD_BYTES], elapsed[PROFILE_FIELD_BYTES];
} profile_cgminer;

typedef struct {
    char battery[PROFILE_ENTITY_BYTES], temperature[PROFILE_ENTITY_BYTES], actuator[PROFILE_ENTITY_BYTES];
    char outlet[PROFILE_ENTITY_BYTES], grid[PROFILE_ENTITY_BYTES];
    char outlet_watts[PROFILE_ENTITY_BYTES], outlet_energy[PROFILE_ENTITY_BYTES];
} profile_home_assistant;

typedef union {
    profile_shelly_gen1 gen1;
    profile_shelly_rpc rpc;
    profile_tasmota tasmota;
    profile_victron victron;
    profile_fronius fronius;
    profile_cgminer cgminer;
    profile_home_assistant home;
} integration_profile;

const char *integration_profile_name(profile_family family);
profile_family integration_profile_family(const char *name);
bool integration_profile_decode(const char *text, json_record *record,
                                profile_family *family, integration_profile *profile);
// Install parsed profiles only at startup, before the worker and HTTP server use them.
bool integration_profile_activate(profile_family family, const integration_profile *profile);
const void *integration_profile_get(profile_family family);
uint64_t integration_profile_digest(profile_family family, const void *mapping);
uint64_t integration_profile_revision(profile_family family);

#endif
