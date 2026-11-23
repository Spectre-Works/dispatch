<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# DISPATCH/01 controller

This is the ESP-IDF project for the AtomS3-Lite development target.
[main/installation.c](main/installation.c) owns polling, saved installation
state, and actuation. [main/web.c](main/web.c) serves the embedded dashboard
and local API. The project compiles the shared C policy and production
integrations directly.

From the repository root in an activated ESP-IDF v6.1.1 shell:

```sh
idf.py -C apps/controller build
```

Use the [development guide](../../docs/development.md) for selecting the
USB device, first flash, routine application updates, and provisioning.
