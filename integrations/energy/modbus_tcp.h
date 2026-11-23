// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_MODBUS_TCP_H
#define DISPATCH_MODBUS_TCP_H

#include "shelly.h"

enum { MODBUS_TCP_WORDS = 8 };

shelly_result modbus_read(const char *ip, uint16_t port, uint8_t unit, uint16_t address,
                          uint16_t words[], unsigned count, int64_t deadline);
shelly_result modbus_write(const char *ip, uint16_t port, uint8_t unit, uint16_t address,
                           const uint16_t words[], unsigned count, int64_t deadline);
#endif
