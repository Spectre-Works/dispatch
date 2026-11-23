<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# DISPATCH/01 hardware

The controller targets the AtomS3-Lite with an ESP32-S3, 8 MiB of embedded
flash, internal RAM, and USB Serial/JTAG. The [partition table](partitions.csv)
reserves two application slots of 0x3f0000 bytes.

See [build, flash, and recovery](../../docs/development.md#build-and-flash).
