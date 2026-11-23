// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_TASMOTA_H
#define DISPATCH_TASMOTA_H

#include "shelly.h"
#include "json_record.h"

// Wi-Fi MAC address reported by Status 5, uppercase hex with separators removed.
bool tasmota_valid_identity(const char *mac);
shelly_result tasmota_status(const shelly_target *target, unsigned channel, shelly_sample *sample);
shelly_result tasmota_set(const shelly_target *target, unsigned channel, bool on, bool (*still_allowed)(void));

// Decode response records common to the direct HTTP and broker paths.
bool tasmota_matches(const json_record *doc, const char *identity);
bool tasmota_output(const json_record *doc, unsigned state, unsigned channel, bool *on);
bool tasmota_observed_output(const json_record *doc, unsigned state, unsigned channel, bool *on);
bool tasmota_only_output(const json_record *doc, unsigned state);
void tasmota_meter(const json_record *doc, shelly_sample *sample);
#endif
