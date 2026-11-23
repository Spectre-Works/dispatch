<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Production integrations

| Integration | Current controller capability |
| --- | --- |
| [Shelly](switches/README.md) | Generation-specific outlet status, metering, relay control and timed tests where supported |
| [Tasmota HTTP and MQTT](../docs/integration-setup.md#pair-an-outlet) | Identified local relay state and control, with power and energy on supported metered profiles |
| [Home Assistant REST](../docs/integration-setup.md#set-up-home-assistant) | Switches, mapped meter and battery/temperature sensors, and selected number/select/restart-button actions |
| [cgminer](miners/README.md#initial-read-only-adapter) | Read-only miner reachability, recent hashrate, and accepted-share counter |
| [Goldshell](miners/README.md#goldshell-blake2b-power-modes) | Model and firmware-specific factory presets, native Idle, and restart where supported |
| [APsystems ECU-R](energy/README.md#apsystems-ecu-r) | Reported generation used to estimate headroom after a configured reserve |
| [Whole-site grid sources](energy/README.md#whole-site-grid-flow) | Shelly, Victron GX, Fronius Solar API, SunSpec Modbus TCP, Solarman V5, and Home Assistant signed grid readings |
| [Victron GX ESS](../docs/integration-setup.md#verify-grid-flow-and-set-an-ess-target) | Bounded volatile grid target with live preflight and readback |
| [XBT and CONVOY DATUM](mining-services/README.md) | Optional read-only readiness, checkpoint, and mining path evidence |

See the [compatibility matrix](../docs/compatibility.md) for exact profiles, evidence and provenance.
Keep equipment-specific behavior here and product decisions in the
[operating core](../core/README.md).
