// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_LOCAL_HTTP_H
#define DISPATCH_LOCAL_HTTP_H
#include "json_record.h"
#include <stdbool.h>
#include <stddef.h>

// Shared serially by the installation worker. A request replaces the previous body.
char *local_http_body(void);
const json_record *local_http_json(void);
int local_http_response(const char *ip, unsigned port, const char *header, size_t header_bytes,
                        const char *payload, size_t payload_bytes, bool prefix);
int local_http_response_until(const char *ip, unsigned port, const char *header, size_t header_bytes,
                              const char *payload, size_t payload_bytes, bool prefix, int64_t deadline);
#endif
