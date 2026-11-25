// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_EQUIPMENT_WRITES_H
#define DISPATCH_EQUIPMENT_WRITES_H

#include <stdbool.h>

static inline __attribute__((always_inline)) bool equipment_writes_allowed(void)
{
#ifdef DISPATCH_PREVIEW_ONLY
    return false;
#else
    return true;
#endif
}

#endif
