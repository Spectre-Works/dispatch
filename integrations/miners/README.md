<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Miner Integrations

Implement adapters for explicitly supported models/firmware. Distinguish
reachability, reported mining activity, accepted work, restart capabilities,
and optional operating profiles. Do not imply every model supports each feature.

## Initial read-only adapter

The controller sends `{"command":"summary"}` to a configured local IPv4
address on TCP 4028. It accepts a bounded cgminer JSON summary, reading
`SUMMARY[0].Accepted`, the first valid rate from `MHS 5s`, `MHS 20s`, or
`MHS av`, and `Elapsed` when the miner reports a valid whole-second
mining process runtime. Only a recent rate is used to learn startup time
and running power. A valid zero is displayed as zero. Accepted is a
miner-reported counter with firmware-specific reset behavior. Independent
acceptance by the intended mining service requires a separate data source.

This adapter targets the documented cgminer JSON summary protocol. It sends
no privileged requests, firmware changes, profile changes or restart
requests. Individual firmware forks and models need hardware validation.
[Upstream cgminer API documentation](https://github.com/ckolivas/cgminer/blob/master/API-README)

## Goldshell Blake2b power modes

Goldshell documents Default and Low-Power modes for the HS Lite starting
with firmware 2.2.3. This applies to both of its algorithms. The vendor's
local interface puts power-plan selection under **Miner → Miner**.
Changing the power plan on the existing algorithm is a separate operation
from changing algorithms or pool settings.

DISPATCH automatically reads the model and firmware from `/mcb/status`
at each configured miner IP while the associated outlet has a fresh
powered-on sample. The installation worker checks one address at a time
after polling and control work, with at least 15 seconds between
background checks. The worker rotates through due slots and rechecks
settling miners sooner than stable ones. The Equipment view reports the
identified model and detected factory presets while live confirmation is
pending. It can display the name of a newer Goldshell miner before a
control profile exists.

| Model | Firmware | Factory hashing presets | Expected hashboards | Validation |
|---|---|---|---:|---|
| HS Lite in SC mode | 2.2.5 | Default 800 / Low-Power 650 MHz | 2 | Live readback and preset control |
| HS Lite in SC mode | 2.2.3, 2.2.4 | Default 800 / Low-Power 650 MHz | 2 | Experimental, public firmware and synthetic fixtures |
| SC BOX II | 2.2.2 | Default 690 / Low-Power 525 MHz | 1 | Experimental, public firmware and synthetic fixtures |
| HS BOX II in SC mode | 2.2.6 | Default 850 / Low-Power 638 MHz | 1 | Experimental, public firmware and synthetic fixtures |
| KD5 Pro in SC mode | 2.2.3, 2.2.4 | Default 825 / Balance 700 MHz | 3 | Experimental, public firmware and synthetic fixtures |
| SC5 Pro, hardware 30.50.SA | 2.2.0 | Default 840 MHz / Low-Power from factory settings | 4 | Experimental, public live telemetry and synthetic writes |
| ARI31, hardware 30.50.SA, in SC mode | 2.2.3 | Default 525 MHz / Balance or Low-Power from factory settings | 4 | Experimental, public live telemetry and synthetic writes |
| SC5 Pro II | Recognized live firmware layout | Default / Low-Power from factory settings | Checked against rated output | Experimental, published ratings and synthetic writes |
| CAEU04, CAEU14, SC Ultra in SC mode | Recognized live firmware layout | Default / Balance or Low-Power from factory settings | Checked against listed output | Experimental, reseller ratings and synthetic writes |

Inspection checks the model, firmware, settings schema, algorithm on
dual-algorithm models, factory preset values, and active clock over TCP
4028 (`devs`). Every expected hashboard must appear once, be alive,
report the same factory clock, and report enabled if the firmware sends
an enabled field. The dashboard shows a differing saved preset separately.
For a manual switch, enter the miner's local dashboard password. For
automatic switching, authorize the compatible miner in **Managed presets**
and choose its preset preference. Managed switching requires two qualifying
hashing presets and the same live identity, settings, and board checks
used for manual control. The validation column records the evidence
available for each firmware. The adapter checks the model, firmware, and
active preset again before an authenticated
`PUT /mcb/setting`. It confirms the saved selection and live board clocks
afterward. If a miner needs time to settle, read-only checks repeat every
five seconds for up to 30 seconds. Only the first request can send a
write. Manual credentials and session tokens are temporary. The controller
holds managed authorization locally. Boards with the required hardware
key can keep it across restarts. The vendor interface uses HTTP on the
equipment LAN.
[Per-miner presets and daylight behavior](../../docs/operating-policy.md#presets)

The adapter changes only the factory preset selection. It sends no
algorithm, pool, voltage, frequency, or temperature-monitor changes.
The outlet policy continues to govern relay decisions. After readback
confirms a change, DISPATCH starts new efficiency and running-power
observations for the new preset.

[Goldshell LITE dual-mode firmware announcement](https://www.goldshell.com/upgrade-your-goldshell-lite-series-miner-firmware-to-access-dual-mode-hash-rates/) ·
[Goldshell local power-plan tutorial](https://goldshellhelp.zendesk.com/hc/en-us/articles/17163031461017-Tutorial-on-Switching-Power-Plan) ·
[Goldshell BOX series tutorial](https://goldshellhelp.zendesk.com/hc/en-us/articles/17608608063513-BOX-Series-Tutorial)

## Goldshell native restart

From the miner's **Power mode** dialog, choose **Restart miner** and
enter its local dashboard password. This operation requires a current
miner identity and elapsed reading, a confirmed powered outlet, and an
allowed operating period. DISPATCH sends the stock dashboard's native
restart request once and leaves the outlet under the operating plan.

| Model | Stock firmware | Status model |
| --- | --- | --- |
| KD BOX | 2.2.4 | `Goldshell-KDBox` |
| HS BOX | 2.2.6 | `Goldshell-HSBox` |
| HS Lite | 2.2.5 | `Goldshell-HSLITE` |

The controller rechecks the exact model, firmware, and hardware reported
for the configured miner before authentication. It erases the one-time
password and session token after the request. The HTTP acknowledgment and
the newly observed mining process are separate. A reset of the reported
elapsed counter within the verification window confirms that a new mining
process was observed. A missing or inconclusive observation leaves the
request unconfirmed. Inspect the miner's dashboard before issuing another
restart. When Wi-Fi or the outlet changes during verification, the
controller ends verification without replaying the request.

The three stock firmware archives and synthetic command, authorization,
and readback fixtures establish the protocol path. The physical restart
operation is still awaiting validation. Factory presets and Idle have
separate compatibility requirements. [Firmware provenance](../../docs/compatibility.md#reproduce-the-goldshell-protocol-check)

## Runtime qualification

For profiles without a verified factory board count, DISPATCH checks
contiguous board IDs, live state, matching clocks, and recent positive
hashrate on each board, within the four-board response limit. The clocks
identify the preset. Aggregate hashrate must be within 5% of the listed
rating for that preset before control is offered. A queued write also
rechecks the hardware version, firmware, board count, and both clocks.
Physical settings and writes on these runtime-qualified profiles still
need verification.

## Native Idle support

Native Idle stops hashing while leaving the miner powered and reachable.
The Power mode dialog offers **Pause hashing** and explicit resume
choices for the experimental profiles below. Their firmware images
contain a zero-clock level 3 entry. The vendor dashboard displays Idle
for status code 6 from its web `devs` request.

| Model | Firmware | Preset schema | Default in SC mode | Hashboards |
|---|---|---|---:|---:|
| SC BOX | 2.2.5 | v2.1, flat | 725 MHz | 1 |
| HS BOX | 2.2.6 | v2.1, grouped | 850 MHz | 1 |
| HS3 | 2.2.7 | v1.2, flat | 750 MHz | 3 |
| HS3 SE | 2.2.4 | v1.2, flat | 750 MHz | 4 |
| HS5 | 2.2.5 | v1.2, flat | 750 MHz | 4 |
| HS6 | 2.2.8 | v1.2, grouped | 715 MHz | 4 |
| HS6 SE | 2.2.7 | v1.2, grouped | 825 MHz | 3 |
| SC6 SE | 2.2.2 | v1.1, flat | 650 MHz | 4 |
| KD5 | 2.2.6 | v1.2, grouped | 800 MHz | 4 |
| HS Lite | 2.2.5 | v1.2, grouped | 800 MHz | 2 |
| SC BOX II | 2.2.2 | v1.3, flat | 690 MHz | 1 |
| HS BOX II | 2.2.6 | v1.2, grouped | 850 MHz | 1 |
| KD5 Pro | 2.2.4 | v1.2, grouped | 825 MHz | 3 |

For HS3, HS3 SE, and HS5 the selected algorithm must also be confirmed through
`/mcb/algosetting`. To confirm Idle, DISPATCH checks the saved selection
and web Idle status, and checks for recent positive hashrate through the
miner API. Resume is confirmed with the matching live board clocks.
Confirmed Idle pauses recovery and missing-work monitoring while the
outlet continues to follow the operating plan. The Idle and resume write
paths have been exercised with synthetic fixtures. The HS3 SE profile
uses the archived daemon's level 3 to web status 6 mapping, plus the
archive's preset and four-board configuration. Physical switching needs
verification on these profiles.

## Inspect preset evidence

The read-only [preset evidence collector](../../tools/dispatch_miner_presets.py)
can run from any host on the miner's LAN with Python 3. Run it once in
each factory preset after the board clocks settle. The JSON includes the
reported model, firmware, selected algorithm, factory preset levels,
configured selection, active hashboard clocks, and the stock dashboard's
separate board status.
On the HS Lite the dashboard's `powerplan` field matches the saved
setting even when live clocks show another preset. The collector labels
this field separately and omits addresses, pools, and credentials.

```sh
python3 tools/dispatch_miner_presets.py 192.168.1.20
```

Sources: [Goldshell firmware archive index](https://github.com/goldshellminer/firmware),
[BOX specifications](https://goldshellhelp.zendesk.com/hc/en-us/articles/17466093334809-BOX-Series-Miner-Specifications),
[HS BOX II](https://www.goldshell.com/dual-algorithms-optimize-returns/),
[MAX specifications](https://goldshellhelp.zendesk.com/hc/en-us/articles/17486276261017-MAX-Series-Specifications),
[asic-rs live SC5 Pro and ARI31 captures](https://github.com/256foundation/asic-rs/tree/d8adbeb46e778f19212558c14d13fb961d4d8b55/asic-rs-firmwares/goldshell/tests/fixtures),
[Reseller-listed operating profiles](https://zhenchainmicro.com/products/used-goldshell-sc-ultra-24th-s-siacoin-blake2b-miner),
[SC Lite](https://www.goldshell.com/product/goldshell-sc-lite/),
[SC Lite local API captures](https://github.com/BitcoinMechanic/goldshell-config),
[MINI DOGE III](https://www.goldshell.com/product/goldshell-mini-doge-iii/),
[MINI DOGE III Plus](https://www.goldshell.com/product/goldshell-mini-doge-iii-plus/),
[BYTE tutorial](https://goldshellhelp.zendesk.com/hc/en-us/articles/45029717654937-Goldshell-BYTE-Tutorial),
and [AE BOX II](https://www.goldshell.com/product/goldshell-ae-box-ii/).
