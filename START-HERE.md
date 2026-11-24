<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->

# Start here

## Get to Preview

1. **Gather the hardware.** Use an AtomS3-Lite with 8 MiB of flash, a USB data
   cable, a trusted 2.4 GHz Wi-Fi network, and one
   [compatible metered outlet](docs/compatibility.md) and miner. Keep the
   controller, outlet, and miner reachable from the same LAN during setup.
2. **Build and flash.** Install and activate ESP-IDF v6.1.1, then follow
   [Build and flash](docs/development.md#build-and-flash). Use the initial
   flash instructions for a blank board. On an existing installation,
   follow the instructions for its active app slot.
3. **Pair over USB and join Wi-Fi.** Run the
   [USB pairing tool](docs/development.md#provision-the-controller). Use
   the **12-digit setup key** on the temporary DISPATCH Wi-Fi. Enter the
   target Wi-Fi credentials and the **32-character owner key** from the
   USB tool. Keep the owner key in a private file or password manager.
4. **Open the LAN dashboard.** Rejoin your usual Wi-Fi and open the
   `http://dispatch-….local/` address shown in setup. If the name is
   unavailable, use the DHCP address listed in the router or USB console.
   Sign in with the owner key. On a personal device you can choose
   **Remember this browser**. The dashboard uses HTTP on the trusted LAN.
5. **Pair one outlet in Preview.** In **Settings → Equipment & plan →
   Pair equipment**, enter the outlet and miner details and save in
   Preview. Confirm the physical mapping under **Equipment → First-run
   checks**. Save an operating plan, then inspect **Outlet Preview** on
   Overview. Preview shows what would happen. Enable outlet control
   only after reviewing those decisions.

[Setup and recovery](docs/development.md#recover-setup-or-retire-a-controller) · [Operating guide](docs/operations.md)

## Other starting points

| I want to… | Start with… |
| --- | --- |
| Use a provisioned DISPATCH/01 | [Operator guide](docs/operations.md) |
| Set up Wi-Fi on a development controller | [USB pairing and first sign-in](docs/development.md#provision-the-controller) |
| Check a checkout without hardware | [Host checks](docs/development.md#checks-and-simulation) |
| Build and flash a development controller | [Toolchain, build, and USB flash](docs/development.md#build-and-flash) |
| Understand or contribute to the project | [README](README.md), [contributor guide](CONTRIBUTING.md), and [architecture](docs/architecture.md) |
| Review applicable license terms | [LICENSING.md](LICENSING.md) and the [operative license](LICENSE) |
