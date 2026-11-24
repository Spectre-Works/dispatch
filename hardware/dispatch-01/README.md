<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# DISPATCH/01 hardware

The controller targets the AtomS3-Lite with an ESP32-S3, 8 MiB of embedded
flash, internal RAM, and USB Serial/JTAG. The [partition table](partitions.csv)
keeps the two application slots at 0x20000 and 0x410000 with 3.94 MiB
and 3.25 MiB of space respectively.

The 544 KiB archive rotates five-minute rows across 31 sectors, 15-minute
rows across 40 sectors, hourly rows across 41 sectors, and events across
24 sectors. The 160 KiB operations journal rotates ten four-sector groups.
At continuous five-minute logging, each five-minute sector is erased about
170 times per year. Journal erase frequency depends on how many blocks
change between saves. Wi-Fi settings, profiles, and credentials use NVS.

The earlier four-slot NVS state is imported on first boot. The archive
reader also recognizes the earlier activity partition. A partition table
update is required when upgrading from the four-slot layout.

See [build, flash, and recovery](../../docs/development.md#build-and-flash).
