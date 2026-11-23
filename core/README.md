<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Operating core

Portable C implements the rules used by DISPATCH/01.

- [dispatch_policy.c](dispatch_policy.c) evaluates the operating plan, paid
  permissions, allowance guard, relay decisions, and adaptive presets.
- [dispatch_daylight.c](dispatch_daylight.c) calculates local daylight
  boundaries used by gap coverage.
- [dispatch_evidence.c](dispatch_evidence.c) accumulates observed equipment
  behavior and evaluates learned measurements.

Keep networking, storage, and browser concerns in their respective layers.
Use [the operating policy](../docs/operating-policy.md)
for the policy contract. Run `./tools/check.sh` from the repository root.
