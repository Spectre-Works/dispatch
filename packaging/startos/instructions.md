<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# DISPATCH Preview

1. Start the service, then open **Actions → Show Owner Key** and copy the key.
2. Open **DISPATCH Dashboard** and unlock it with that key.
3. Under **Settings → Equipment & plan**, enter your equipment's LAN
   address and device ID, then confirm the physical mapping from a fresh reading.
4. Set the operating plan and review the proposed actions on **Overview**.
   The dashboard shows proposed actions beside actual equipment readings.
5. Use **Settings → Transfer → Export private backup** when you are ready
   to import the plan on a DISPATCH/01 controller.

StartOS manages the network, system clock, and package updates. Restart the
service after staging an integration profile. StartOS backups include the
volume containing the owner key, equipment credentials, settings, and history.
Keep backups private.
