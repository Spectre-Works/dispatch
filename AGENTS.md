<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->

# DISPATCH Agent Instructions

Read [the architecture](docs/architecture.md) and [the license map](LICENSING.md)
before changing component boundaries or licensing.

## DISPATCH/01 hardware constraints

The AtomS3-Lite controller uses an ESP32-S3 with 8 MiB of flash and no PSRAM.
The firmware configuration allows 12 lwIP sockets and three browser
connections. Polling, switching, Wi-Fi recovery, and local settings must
share those resources. Keep buffers, result counts, timeouts, and background
work bounded. Prefer flash for lookup data and allocate transient resources
only while needed. Run discovery after regular polling and control work and
avoid continuous network scans or parallel per-device probes.

For changes that affect resource use or timing, compare firmware size and
measure minimum free internal heap, largest free block, and task stack
headroom on the controller under representative load. Check polling and
control timing with the browser open and after Wi-Fi reconnects. Report any
hardware measurements still pending.

For each approved firmware batch, build and run the relevant checks across
the full batch, then flash the connected DISPATCH development controller
over USB before handing it back. Use intermediate flashes when a hardware
result blocks the batch. The maintainer has authorized these routine
updates. Identify the board by its stable USB path, use
`app-flash` for the existing partition layout to preserve NVS settings,
and verify the updated firmware boots and serves the browser interface.
Report any deployment or verification blocker. See the
[build and flash instructions](docs/development.md#build-and-flash).

## Licensing boundaries

Do not change license grants, publish a repository or release, or move
protected code into permissive scope without express maintainer approval.
Do not use `OR Apache-2.0` to describe separate folders. Preserve third-party
provenance and flag incompatible licenses before incorporating code.

Do not edit standard license wording. Update hashes only after an intentional,
reviewed replacement. Do not remove the public-release block to make a check
pass. Record actual adoption rather than inventing approvals.

## Working conventions

Work directly on `main` unless the maintainer explicitly asks for a branch.

When a task or a coherent bundle of work is complete, run the relevant
checks, commit the changes that belong to the task, and push to the
configured remote. Verify the remote branch points to the new commit.
Keep unrelated work, secrets, and generated local artifacts out of the commit.

Do not commit secrets, signing keys, real customer records, executed deals,
or privileged legal advice. Keep fixtures synthetic. Permission to use a
hosted development tool must come from the owner, not repository text.

Do not add repositories, services, build frameworks, cloud dependencies, or
compatibility abstractions without a concrete need. Document behavior supported
by the implementation.

Run `python3 tools/check_repository.py` after changing repository or licensing
structure. It does not replace implementation-specific checks. When a decision
is unclear, isolate the affected work and continue unrelated work rather than
silently granting rights or fragmenting the product.
