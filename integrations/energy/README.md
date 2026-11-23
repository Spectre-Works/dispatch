<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Energy Integrations

Read supported existing energy sources. Distinguish solar generation from
measured available surplus and forecasts. Include source freshness and
availability rather than turning missing readings into zero. No new mains
measurement hardware is assumed.

## APsystems ECU-R

The controller queries the local ECU-R summary over TCP, usually port 8899.
It reads production, daily and lifetime energy, inverter counts, identity,
and firmware from the summary. DISPATCH implements the framing described
in the [local protocol notes](https://github.com/npeter/ioBroker.apsystems-ecu#apsystems-ecu-protocol---command-group-11)
and verifies it on the target ECU-R. Each connection has a five-second
deadline. At most one connection is open for the ECU-R and one retry is
scheduled per cycle. Default polling is five minutes during the day and
twenty minutes overnight. The controller keeps the last successful sample in
RAM and marks it as aged after fifteen minutes. The operating plan can use
this production to estimate headroom with paired miner outlets and an
owner-selected reserve. The summary carries no inverter observation
timestamp. The control logic checks for progress in successive summaries
and blocks starts when recent progress cannot be established. Reported
production and estimated headroom are separate from grid import and export.

## Whole-site grid flow

The worker polls one configured grid source on a three-second target cadence.
A failed read or connection change immediately invalidates the reading for
control. A successful reading expires after twelve seconds. Miner outlets
continue to report their own consumption. Home Assistant supplies a single
selected signed-power entity. Fronius Solar API supplies signed grid flow
with a grid-side meter and reports production separately.

Shelly Pro 3EM uses [EM.GetStatus](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/EM/).
Original Shelly 3EM uses the three documented phase measurements. Shelly Pro
EM uses a selected EM1 channel. Victron GX uses a bounded Modbus TCP transport
with a specific profile and a separate volatile ESS setpoint operation. See
[setup and commissioning](../../docs/integration-setup.md#verify-grid-flow-and-set-an-ess-target).
