<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Switch Integrations

Use supported local interfaces to complete, unmodified switching products.
Expose real state, power measurements, and action outcomes where available.
Do not equate outlet power with successful mining or claim universal PSU/load
compatibility from a resistive rating.

## Initial Shelly RPC adapter

The initial equipment target is Shelly Plus Plug US with metering on
channel 0. Supply its exact local device ID and IPv4 address. DISPATCH uses
`Switch.GetStatus` for relay state, instantaneous power and cumulative
energy, and `Switch.Set` with explicit desired state. The adapter checks the
response source ID, performs a status preflight before sending a Set, and
supports local HTTP Digest with SHA-256 when enabled. For each desired state,
the controller waits for a later status observation before treating it as
confirmed. [Shelly Plus Plug US](https://shelly-api-docs.shelly.cloud/gen2/Devices/Gen2/ShellyPlugUS/),
[Shelly Switch RPC](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/Switch/),
[Shelly authentication](https://shelly-api-docs.shelly.cloud/gen2/General/Authentication/)
