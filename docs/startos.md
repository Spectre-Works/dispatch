<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# DISPATCH Preview for StartOS

DISPATCH Preview observes your equipment and shows the actions DISPATCH/01
would take under your operating plan. The dashboard displays those proposals
alongside the relay, meter, and miner states it has actually observed. An Atom
DISPATCH/01 controller runs active equipment control.

## Install and connect

1. Use StartOS **0.4.2 or newer** on an x86_64 or aarch64 node. Download the
   matching `.s9pk` and `DISPATCH-STARTOS-SHA256SUMS` from the DISPATCH
   release. Check the package checksum and compare its signer with the
   maintainer's published StartOS signing identity.
2. Open **Sideload** in StartOS, select the package, review its signing
   identity, and install. Start **DISPATCH Preview** and wait for its dashboard
   and observation worker health check.
3. Under **Actions → Show Owner Key**, copy the owner key. Open **DISPATCH
   Dashboard** from the service and enter the key.
4. Use **Settings → Equipment & plan → Pair equipment** to enter the private
   IPv4 address and device ID. Pair one equipment slot at a time. Confirm
   what each outlet powers once the dashboard receives a fresh reading.
5. Set the operating plan and review **Overview**, **Energy**, and **Activity**
   for proposed actions, their reasons, and actual observations.

The service uses the StartOS system clock. Set the installation time zone and
equipment credentials in DISPATCH. Enter each equipment address explicitly.
The node must have a route to the equipment and the equipment must allow
connections from the node. Use the same [model and firmware
requirements](compatibility.md) and [commissioning steps](integration-setup.md)
as DISPATCH/01. Use a matching broker hostname and CA for MQTT TLS. A fresh
status query identifies the device and reads the outlet. The service blocks
equipment commands, timed power tests, and outbound notification webhooks.

[StartOS sideloading](https://docs.start9.com/start-os/0.4.2.x/sideloading.html) · [Start SDK 4.1.0 requirements](https://github.com/Start9Labs/start-technologies/releases/tag/start-sdk/v4.1.0)

## Transfer, update, and recover

Use **Settings → Transfer → Export private backup** to move your operating plan
and equipment slot addresses and IDs to DISPATCH/01. On the controller,
choose **Apply in Preview**, re-enter the required credentials, and review
mapping and meter direction before enabling physical control. Use the
[configuration transfer field list](configuration-transfer.md) to identify
settings that require separate setup.

Use StartOS sideloading to install a newer package with the same signer and
package ID. The persistent data volume carries forward the owner key,
equipment configuration, profile uploads, accounting, and activity. Stop and
start the service in StartOS after staging an integration profile. Recover the
current dashboard key from **Actions → Show Owner Key**.

Use StartOS backup and restore to recover the entire service volume. Protect
the backup and its password because the volume includes equipment credentials
and the dashboard owner key. Keep the compatible package and signer identity
available for recovery. When replacing an installation, keep a separate
configuration export for moving only the plan and equipment slots.

[StartOS backups](https://docs.start9.com/start-os/0.4.2.x/backups.html) · [Build and release DISPATCH packages](../packaging/README.md#startos-packages)
