<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# DISPATCH architecture

DISPATCH/01 runs the operating plan on an ESP32-S3. The browser sends owner
intent and displays observations, decisions, and history. The controller
continues to poll and act when the browser closes.

```text
Browser dashboard (apps/web)
        │ authenticated LAN HTTP
        ▼
Controller HTTP routes (apps/controller/main/web.c)
        │ snapshots and owner intent
        ▼
Installation worker (apps/controller/main/installation.c)
        ├─ core/          policy, accounting, daylight, equipment evidence
        ├─ integrations/  meter, outlet, miner, and mining-service protocols
        └─ local storage  settings, budgets, profiles, and activity history
                   ▲
       validated owner profile uploads in Settings
```

## Where changes belong

| Change | Start with |
| --- | --- |
| Allowed operation or an explanation for waiting, starting, or stopping | [Policy engine](../core/dispatch_policy.c) and its [host tests](../tests/dispatch_policy_test.c) |
| How an outlet, miner, energy source, or mining service is queried or controlled | [Protocol adapters](../integrations/README.md) |
| Reading freshness, worker scheduling, command confirmation, and saved installation state | [Installation worker](../apps/controller/main/installation.c) |
| Local HTTP interface, status responses, and validation of owner requests | [HTTP routes](../apps/controller/main/web.c) |
| Dashboard interactions and presentation | [Browser interface](../apps/web/README.md) |
| Five-minute activity and event retention | [Timeline storage](../apps/controller/main/timeline.c) |

## From readings to actions

The worker polls one external endpoint at a time. Adapters report
measurements, freshness, capabilities, and outcomes from the equipment.
The worker assembles inputs and runs the portable policy engine. The same
engine evaluates Preview and active control. A proposed action, a request
sent to equipment, and a confirmed observation are distinct states.

DISPATCH keeps settings, budget progress, and learned evidence locally.
Activity stores recent intervals and events in a bounded flash ring. Missing
measurements appear as gaps. Keeping the policy independent of the SDK makes
its behavior testable without hardware.
[Operating plan rules](operating-policy.md) · [Host checks](development.md#checks-and-simulation)

## Platform boundaries

Keep device quirks in the adapters, operating rules in the core, and transport,
storage, and lifecycle in the controller application.

## SDK boundary

The [DISPATCH SDK v0.1.0](https://github.com/Spectre-Works/dispatch-sdk/tree/v0.1.0)
publishes the v1 profile contract, stock mappings, validator, and LAN bridge.
The firmware bundles the matching stock data and supports local uploads of
compatible profiles. At boot the loader selects one mapping per family
before starting the worker. The worker retains the native transport,
identity checks, and command reconciliation.

A custom bridge runs on a separate host and uses the existing authenticated
REST connection for selected entities and a versioned solar production route.
The browser sends owner intent through the regular
installation routes. DISPATCH polls, authorizes, and reconciles bridge
actions through the same worker used for other equipment.
