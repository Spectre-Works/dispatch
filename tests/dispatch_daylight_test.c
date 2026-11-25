// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "dispatch_daylight.h"

#include <assert.h>
#include <stdio.h>

static void sunset_history(void)
{
    int16_t shift;
    int16_t earlier[] = {-2400, 2400, -3000, -2700, -1800, 900};
    assert(dispatch_sunset_shift(earlier, sizeof(earlier) / sizeof(*earlier), &shift) && shift == -2400);
    int16_t later[] = {2700, 1800, 900};
    assert(dispatch_sunset_shift(later, 3, &shift) && shift == 1800);
    assert(!dispatch_sunset_shift(later, 2, &shift));
    int16_t split[] = {2700, 2400, 2100, -900, -1800, -1200};
    assert(!dispatch_sunset_shift(split, 6, &shift));
    int16_t scattered[] = {-5400, -2700, 0, 2700, 5400};
    assert(!dispatch_sunset_shift(scattered, 5, &shift));
}

int main(void)
{
    const int64_t november_21 = 20778;
    const int64_t june_21 = 20625;
    const int64_t noon_utc = november_21 * 86400 + 12 * 3600;
    const dispatch_daylight denver = dispatch_sun_times(november_21, 397392, -1049903);
    assert(denver.kind == DAYLIGHT_RISE_SET);
    // USNO tables give 06:51 and 16:40 MST for Denver on this date.
    const int rise = (denver.sunrise - november_21 * 86400) / 60 - 420;
    const int set = (denver.sunset - november_21 * 86400) / 60 - 420;
    assert(rise >= 409 && rise <= 415 && set >= 998 && set <= 1006);
    assert(!dispatch_in_daylight(denver, denver.sunrise - 1));
    assert(dispatch_in_daylight(denver, denver.sunrise));
    assert(dispatch_in_daylight(denver, denver.sunset - 1));
    assert(!dispatch_in_daylight(denver, denver.sunset));
    assert(dispatch_solar_day(november_21 * 86400 + 60, -1049903) == november_21 - 1);
    assert(dispatch_solar_day(noon_utc + 600, 1799000) == november_21 + 1);

    const dispatch_daylight auckland = dispatch_sun_times(june_21, -368501, 1747645);
    assert(auckland.kind == DAYLIGHT_RISE_SET);
    assert(auckland.sunrise < june_21 * 86400 && auckland.sunset > june_21 * 86400);
    const int nz_rise = (auckland.sunrise - june_21 * 86400) / 60 + 720;
    // USNO tables give 07:34 NZST for Auckland on this date.
    assert(nz_rise >= 452 && nz_rise <= 458);
    assert(dispatch_in_daylight(dispatch_sun_times(june_21, 785000, 150000), noon_utc));
    assert(dispatch_sun_times(november_21, 785000, 150000).kind == DAYLIGHT_ALL_NIGHT);
    assert(dispatch_sun_times(june_21, 900000, 0).kind == DAYLIGHT_ALL_DAY);
    assert(dispatch_sun_times(june_21, -900000, 0).kind == DAYLIGHT_ALL_NIGHT);
    assert(dispatch_sun_times(november_21, 900001, 0).kind == DAYLIGHT_UNKNOWN);
    assert(dispatch_sun_times(0, 0, 0).kind == DAYLIGHT_UNKNOWN);
    sunset_history();
    puts("DISPATCH solar position and learned sunset tests passed");
}
