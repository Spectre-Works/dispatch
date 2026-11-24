<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# DISPATCH configuration transfer

The browser produces a JSON object with `format` set to
`dispatch-configuration` and `version` set to `4`. The document contains
the saved operating plan and the equipment needed to run it. Keep
it private if the equipment addresses and IDs identify your network.

| Field | Representation |
| --- | --- |
| `timezone`, `offset` | IANA name, or an empty name and a signed fixed offset in minutes from −720 to 840 |
| `start`, `end` | Allowed mining hours in local minutes from 0 to 1439, with equal times allowing a full day |
| `rate`, `allowance` | Default rate in millicents per kWh and daily allowance in millicents. Zero allowance removes the limit |
| `quiet` | Object with boolean `enabled` and local minute `start` and `end` |
| `rateWindows` | Two objects with boolean `enabled` and integer `start`, `end`, and `rate` in the same units |
| `weekend` | Use `{"enabled":false}` for a daily plan. With `enabled` true, include `start`, `end`, `quiet`, and `rateWindows` with the same meanings for Saturday and Sunday |
| `operating` | Boolean `paidEnabled`, local minute pairs `paidStart` and `paidEnd` and `weekendPaidStart` and `weekendPaidEnd`, plus the `gap` object below |
| `equipment` | Eight objects ordered by integer `slot` (0–7), each with string `ip`, `identity`, and optional cgminer address `minerIp`. Use empty strings for unused values |

An enabled paid window with equal start and end times covers the whole day
within allowed mining hours. The `gap` object carries a boolean `enabled`
and, when enabled, `minutes` from 5 to 60 plus signed decimal `latitude` and
`longitude` in degrees. The setup form limits the coordinates to four
decimal places.

In **Settings → Configuration transfer**, choose **Export configuration**
to save a file or **Choose file to import** to review one. **Apply in
Preview** replaces all eight slots and the plan together. Version 3 exports are also
accepted and populate slots 0–3, leaving slots 4–7 empty. A
matching outlet ID and address in the same slot can retain its locally stored
password. Configure Home Assistant and MQTT endpoints and credentials under
**Settings → Connections**. Reapply per-outlet meter mappings under
**Settings → Equipment & plan → Pair equipment**. Save custom integration profile
files separately and upload them in **Settings → Integration profiles**.
Re-enter any credentials required on a different controller. Wi-Fi
credentials, the owner key, outlet passwords, learned profiles, meter
history, and control enablement stay on the original controller. Set up
energy and mining-service connections and review equipment mappings on the
destination before enabling control.

When the weekend plan is enabled, the top-level schedule, Quiet Hours,
and rate windows apply Monday through Friday. The default rate and
daily allowance apply to both day groups.

Use the separate support report to share diagnostics. It omits addresses,
device IDs, and credentials.
