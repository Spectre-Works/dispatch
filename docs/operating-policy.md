<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->

# Operating policy

DISPATCH evaluates the operator's mining hours, Quiet Hours, paid permissions,
and equipment choices as one plan. Templates fill the editor. After setup,
the same fields describe every plan.

## Permissions and priority

- Allowed mining hours apply to all energy sources. Quiet Hours and pause take
  precedence. Starts need a known startup draw, the existing outlet dwell
  and restart lockout, and enough time before the next scheduled stop.
- A paid mining window admits eligible starts and can sustain running miners.
  Daylight gap coverage can sustain running miners until solar recovers, the
  gap ends, the evening cutoff arrives, or the allowance runs out. Both can be enabled.
  The paid window takes priority when they overlap. The meter accrues one cost.
- The daily allowance reserves power for sequential outlet changes and
  fresh meter readings. Paid starts also reserve a minimum run interval.
  Dedicated tests have their existing explicit opt-in and bounded lease.
  Meter timing and rate attribution make the allowance an operating target
  rather than a utility bill.
- Surplus operation remains eligible after the paid allowance is used. Shed
  lower-priority outlets first. At equal priority, retain observed productive
  miners and use measured outlet draw to choose a load that covers the
  shortfall. Starts prefer higher priority, then renewable coverage and a
  smaller startup draw at equal priority.

## Energy evidence

A direction-verified whole-site meter guides control when configured. Its
signed flow shows import or export after other site loads. Reserve additional
export for starts and preset increases. For budgeting, attribute measured
import to the miner fleet up to its observed draw and split that amount by
miner draw. This conservative attribution can include household import.
A production-only installation estimates miner coverage after the configured
reserve for other loads.

Missing or stale energy readings block solar-dependent starts. Running
outlets may wait up to 60 seconds after the last usable decision input,
subject to the allowance. A paid mining window or daylight gap coverage can
sustain running miners during a source outage. When a selected meter becomes
unavailable, DISPATCH continues to label it as the selected source.

Daylight gap coverage opens at the location-based sunrise. The controller
checks the last seven evenings of 15-minute Activity records from its solar
production source. A qualifying evening shows production of at least 20 W
followed by two well-sampled 15-minute periods below 20 W, with no later
production at or above 20 W in the evening check. Missing readings cannot
confirm low output. When at least three evenings and a majority of the
qualifying evenings agree within 30 minutes,
DISPATCH applies their median offset to today's calculated sunset, up to
90 minutes earlier or later. It uses the location-based estimate while the
history is insufficient, after clearing Activity, or after replacing the
solar production source until new history qualifies.

Within daylight hours, gap coverage can keep observed running miners on
while the energy source is unavailable. For the allowance, DISPATCH counts
draw without measured solar coverage as paid. A different outlet last seen
off can miss a reading without blocking this permission for up to 60 seconds
from its latest successful observation. DISPATCH reserves for possible draw
on that outlet. The gap timer, allowance, mining hours, and Quiet Hours still
apply. Solar-dependent starts require fresh energy and outlet readings.

## Presets

An authorized compatible miner can leave its preset as set, adapt, or use
the measured efficiency winner. Leave as set is the default. The measured
winner requires a qualifying comparison. A miner's choice does not affect
other miners.

An adaptive miner can lower into a measured preset to fit otherwise
insufficient surplus. During a scheduled paid window a downshift can also
reduce a sustained shortfall by at least 25 W and 5% of the current draw.
Daylight gaps require a measured efficiency benefit and at least two minutes
remaining. During a gap, DISPATCH estimates how long the remaining allowance
would last at the fleet’s current paid draw and tariff. An eligible preset
can extend that allowance when it would run out before the evening cutoff or
remove paid draw altogether. When an evening cutoff is unavailable, an eligible
preset can still reduce paid draw. The maximum gap duration and budget
reserve govern operation. Restores require sustained solar headroom and
the preset dwell interval. Relay starts wait while a preset write is in flight.

## Review and transfer

The dashboard uses one operating plan. Saving is atomic and opens Preview.
The browser evaluates example decisions before saving and shows live outlet
decisions in Preview afterward.

Configuration version 4 carries the schedule, tariffs, allowance, and paid
permissions, including the coordinates used for daylight coverage when
supplied. Set up integrations and authorize miners on the destination
installation. The firmware reads the current installation format.

The on-device history uses the same decision reasons as the live dashboard.
A five-minute record can include the reason for operation at interval close
and the paid permissions seen alongside fresh energy readings during that
interval. Delayed meter readings can contribute energy totals without
assigning that energy to a permission. A replay estimates behavior from
these averages and cannot reproduce changes within each bucket.
