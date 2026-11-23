// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_ECU_R_H
#define DISPATCH_ECU_R_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t watts, today_centi_kwh, lifetime_deci_kwh;
    uint16_t inverters, online;
    char id[13], firmware[32];
} ecu_reading;

typedef enum { ECU_WAITING, ECU_OK, ECU_NETWORK, ECU_PROTOCOL, ECU_IDENTITY } ecu_result;
typedef struct {
    int fd;
    int64_t deadline;
    size_t sent, used;
    bool connected;
    char frame[256];
} ecu_query;

bool ecu_r_decode(const char *frame, size_t size, ecu_reading *reading);
ecu_result ecu_r_start(ecu_query *query, const char *ip, uint16_t port);
ecu_result ecu_r_step(ecu_query *query, ecu_reading *reading);
void ecu_r_cancel(ecu_query *query);
#endif
