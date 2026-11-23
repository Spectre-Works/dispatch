<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Mining-Service Integrations

Optional pool, local service, and node observations support readiness and
accepted-work checks. Pool accounting and actual payouts are separate.
A missing solo-mining block is not evidence of failed hardware.

DISPATCH/01 makes optional read-only requests to two trusted LAN HTTP
interfaces. Configure them in **Mining path → Set up connections**.

- **XBT node** uses Knots JSON-RPC `getblockchaininfo` and `getblockhash`.
  It checks mainnet sync and pins the XBT checkpoint at height 961640.
  Create a separate RPC user limited to these two methods. Enter the
  private IPv4 address and port and, for a LAN service behind a virtual
  host, the HTTP hostname. Use the service interface, separate from the
  StartOS login. The current transport expects plain LAN HTTP, including
  HTTP Basic RPC credentials. Store the credentials only on a trusted
  network and restrict RPC exposure to the controller.
- **DATUM** reads the CONVOY gateway's dashboard **Decentralized Client
  Stats** on its LAN web interface. Pool counters are aggregate gateway
  observations. Solo mode has no pool counters. DISPATCH only observes
  pooling through DATUM and does not connect to a Stratum pool. The
  miner-facing hostname and port are used only to compare an independent
  read-only cgminer report. A missing or ambiguous report stays unknown.

Status routes display endpoint settings and evidence after owner
authorization. They exclude RPC credentials. Mining-path configuration,
automatic-recovery opt-in, and timed overrides are local to the
controller and excluded from configuration transfers.

[CONVOY dashboard source](https://github.com/CONVOYMining/datum_gateway/blob/master/www/home.html) ·
[XBT Knots LAN RPC interface](https://github.com/Horizon-btc/knots-startos/blob/HEAD/startos/interfaces.ts) ·
[DATUM LAN interfaces](https://github.com/Horizon-btc/datum-gateway-startos/blob/HEAD/startos/interfaces.ts)
