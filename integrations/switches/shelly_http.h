// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_SHELLY_HTTP_H
#define DISPATCH_SHELLY_HTTP_H

#include "shelly.h"
#include <stddef.h>

enum { SHELLY_AUTH_BYTES = 480 };

int shelly_http_until(const char *ip, const char *body, const char *authorization,
                      char *answer, size_t capacity, char *challenge, size_t challenge_capacity, int64_t deadline);
int shelly_get_until(const char *ip, const char *path, const char *credentials,
                     char *answer, size_t capacity, int64_t deadline);
bool shelly_digest(const shelly_target *target, const char *challenge,
                   char authorization[SHELLY_AUTH_BYTES]);
#endif
