// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "dispatch_daylight.h"

#include <math.h>

enum { DAY_SECONDS = 86400, DAY_LIMIT = 365 * 230 };

bool dispatch_solar_location_valid(int32_t latitude_e4, int32_t longitude_e4)
{
    return latitude_e4 >= -900000 && latitude_e4 <= 900000 && longitude_e4 >= -1800000 && longitude_e4 <= 1800000;
}

int64_t dispatch_solar_day(int64_t epoch, int32_t longitude_e4)
{
    // Group the date around local mean solar noon, including near the date line.
    return (epoch + (int64_t)longitude_e4 * 240 / 10000) / DAY_SECONDS;
}

static int year_days(unsigned year)
{
    return 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

dispatch_daylight dispatch_sun_times(int64_t day, int32_t latitude_e4, int32_t longitude_e4)
{
    if (day < 1 || day >= DAY_LIMIT || !dispatch_solar_location_valid(latitude_e4, longitude_e4))
        return (dispatch_daylight){0};
    static const double radians = 0.017453292519943295;
    unsigned year = 1970;
    int64_t ordinal = day;
    while (ordinal >= year_days(year)) ordinal -= year_days(year++);
    // NOAA General Solar Position Calculations, evaluated at local mean noon.
    const double gamma = 360 * radians * ordinal / year_days(year);
    const double c1 = cos(gamma), s1 = sin(gamma), c2 = cos(2 * gamma), s2 = sin(2 * gamma);
    const double equation = 229.18 * (0.000075 + 0.001868 * c1 - 0.032077 * s1 - 0.014615 * c2 - 0.040849 * s2);
    const double declination = 0.006918 - 0.399912 * c1 + 0.070257 * s1 - 0.006758 * c2 +
        0.000907 * s2 - 0.002697 * cos(3 * gamma) + 0.00148 * sin(3 * gamma);
    const double latitude = radians * (latitude_e4 / 10000.0);
    const double center = sin(latitude) * sin(declination);
    const double amplitude = cos(latitude) * cos(declination);
    const double horizon = cos(90.833 * radians);
    if (center - amplitude >= horizon) return (dispatch_daylight){.kind = DAYLIGHT_ALL_DAY};
    if (center + amplitude <= horizon) return (dispatch_daylight){.kind = DAYLIGHT_ALL_NIGHT};

    const double half_day = 4 * acos((horizon - center) / amplitude) / radians;
    const double noon = 720 - 4 * longitude_e4 / 10000.0 - equation;
    return (dispatch_daylight){.kind = DAYLIGHT_RISE_SET,
        .sunrise = day * DAY_SECONDS + (int64_t)round(60 * (noon - half_day)),
        .sunset = day * DAY_SECONDS + (int64_t)round(60 * (noon + half_day))};
}

bool dispatch_in_daylight(dispatch_daylight sun, int64_t epoch)
{
    return sun.kind == DAYLIGHT_ALL_DAY || (sun.kind == DAYLIGHT_RISE_SET && epoch >= sun.sunrise && epoch < sun.sunset);
}
