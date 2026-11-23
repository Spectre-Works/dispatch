// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "dispatch_timezone.h"

#include <assert.h>
#include <stdio.h>

static void check(const dispatch_timezone *zone, int offset, time_t utc,
                  uint32_t expected_day, uint32_t expected_minute)
{
    uint32_t day, minute;
    assert(timezone_local(zone, offset, utc, &day, &minute));
    assert(day == expected_day && minute == expected_minute);
}

int main(void)
{
    const dispatch_timezone *denver = timezone_find("America/Denver");
    const dispatch_timezone *phoenix = timezone_find("America/Phoenix");
    const dispatch_timezone *kathmandu = timezone_find("Asia/Kathmandu");
    const dispatch_timezone *casablanca = timezone_find("Africa/Casablanca");
    assert(denver && phoenix && kathmandu && casablanca);
    assert(!timezone_find("Mars/Olympus"));

    // Spring jumps over 02:00 in Denver. Phoenix stays on standard time.
    check(denver, 0, 1772960399, 20520, 119);
    assert(timezone_transition(denver, 1772960399) == 1772960400);
    assert(timezone_transition(denver, 1772960400) == 1793520000);
    check(denver, 0, 1772960400, 20520, 180);
    check(phoenix, 0, 1772960400, 20520, 120);
    assert(!timezone_transition(phoenix, 1772960400));
    assert(!timezone_transition(NULL, 1772960400));
    // Fall repeats 01:00. The preceding local midnight rolls the budget date.
    check(denver, 0, 1793519999, 20758, 119);
    check(denver, 0, 1793520000, 20758, 60);
    check(denver, 0, 1793512799, 20757, 1439);
    check(denver, 0, 1793512800, 20758, 0);
    check(kathmandu, 0, 1772960400, 20520, 885);
    check(casablanca, 0, 1771761600, 20506, 720);
    check(casablanca, 0, 1775390400, 20548, 780);
    check(NULL, -420, 1772960400, 20520, 120);
    uint32_t invalid_day;
    assert(!timezone_local(denver, 0, 0, &invalid_day, NULL));
    assert(!timezone_local(denver, 0, 2556144000LL, &invalid_day, NULL));
    puts("DISPATCH controller timezone tests passed");
}
