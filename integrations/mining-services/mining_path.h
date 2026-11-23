// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_MINING_PATH_H
#define DISPATCH_MINING_PATH_H
#include <stdbool.h>
#include <stdint.h>

enum { XBT_CHECKPOINT_HEIGHT = 961640 };
typedef enum { PATH_WAITING, PATH_OFFLINE, PATH_AUTH, PATH_INVALID, PATH_SYNCING,
    PATH_WRONG_CHAIN, PATH_UNREADY, PATH_READY, PATH_SOLO } path_status;

typedef struct {
    char node_ip[16], rpc_user[49], rpc_password[97];
    char datum_ip[16], miner_host[64];
    uint16_t node_port, datum_port, miner_port;
    char node_host[64], datum_host[64];
} mining_settings;

typedef struct {
    path_status status;
    uint32_t blocks, headers;
} node_reading;

typedef struct {
    path_status status;
    uint64_t accepted, rejected;
    bool counters_known;
} datum_reading;

node_reading xbt_node_status(const mining_settings *settings);
path_status xbt_node_identity(const mining_settings *settings);
datum_reading datum_gateway_status(const mining_settings *settings);
node_reading xbt_decode_status(const char *body);
path_status xbt_decode_identity(const char *body);
datum_reading datum_decode_dashboard(const char *body);

#endif
