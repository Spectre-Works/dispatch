<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Tests and synthetic fixtures

Run `./tools/check.sh` from the repository root. It checks the repository
and compiles and runs the policy, evidence, daylight, time-zone, notification,
and equipment suites on the host. Activate ESP-IDF and run
`./tools/check.sh --with-idf` for the Home Assistant, Tasmota HTTP and MQTT,
and mining-path fixtures that use the pinned SDK HTTP parser.
[Development checks](../docs/development.md#checks-and-simulation)

## Simulate an installation

Use a development-only IPv4 address assigned to the simulator host and
reachable by the controller. The fixture binds TCP 80, TCP 4028, and UDP 123,
so the host needs permission to bind those ports and the addresses must be
available.

```sh
python3 tests/equipment_simulator.py SIMULATOR_IP --control-file /tmp/dispatch-fixture.json
```

1. Pair the fixture's printed device ID and simulator IP in a development
   slot. Use the [fixture's synthetic password](equipment_simulator.py) for
   Digest and the same IP for cgminer. Confirm the synthetic association in
   Preview.
2. For a scheduled-start test, enter an expected startup draw for the fixture
   under **Energy → Solar estimate and equipment power settings**. The
   first fixture reports 500 W when on. Allow paid starts during the current
   mining hours and select **No daily limit** for this synthetic run. Save
   and confirm which outlet Preview proposes to switch on.
3. Exercise control on the selected fixture and compare the simulator's
   requested relay state with the dashboard's confirmed observation. Stop
   the fixture when the run ends.

The controller runs the same policy and adapters against these endpoints.
Multiple explicit IPs create independent synthetic associations. Keep
customer diagnostics, credentials, and actual network captures out of the
fixtures.

The optional control file is a JSON object. Replace it atomically to keep
the fixture from reading a partial update.

| Key | Effect |
| --- | --- |
| `shelly_down` | Close Shelly connections without a response |
| `miner_down` | Close cgminer connections without a summary |
| `reject_sets` | Return HTTP 503 for authenticated Set requests |
| `unconfirmed_sets` | Acknowledge Set and leave the synthetic relay state unchanged |
| `meter_reset` | Rebase the cumulative counter when this nonzero value changes |
| `no_digest` | Serve unauthenticated Shelly requests |

## Simulate Home Assistant and Tasmota

Bind the [bridge simulator](bridge_simulator.py) to an isolated test-network
address reachable from the Atom. It prints synthetic identities and
credentials. Supply a port value of zero to disable an unused listener.

```sh
python3 tests/bridge_simulator.py --bind SIMULATOR_IP --drop-control-ack
```

Use the [setup guide](../docs/integration-setup.md) to configure the fixture.
The Home Assistant adapter can read `sensor.fixture_battery` and
`sensor.fixture_temperature`, then exercise `number.fixture_reserve`,
`select.fixture_preference`, or `button.fixture_restart`. Use the explicit
synthetic endpoint for all control actions. With `--drop-control-ack`, an
accepted Home Assistant action closes the response, so the controller must
confirm its outcome from a later observation.

For MQTT TLS, create a short-lived synthetic CA and a server certificate
with the fixture DNS name as a subject alternative name. Start the
simulator with `--mqtt-port 8883 --mqtt-cert SERVER_PEM --mqtt-key SERVER_KEY`.
Configure the matching name and CA under **Energy → MQTT broker
connection**. Use **Check device status** to validate the broker and
fresh relay state before pairing. A different certificate name or MAC
must fail. Switch exercise through the operating plan needs a separate
development installation with synthetic outlets.

## Wi-Fi reassociation test

For a controller associated with the simulator and left in Preview, build
the opt-in radio test image in a separate build directory. Use the same
partition layout and slot as the default development image.

```sh
idf.py -C apps/controller -B "$PWD/apps/controller/build/link-test" \
  -DSDKCONFIG="$PWD/apps/controller/build/link-test/sdkconfig" \
  -DDISPATCH_LINK_TEST=ON -p "$dispatch_port" app-flash
```

Use the activated ESP-IDF shell and stable USB port from the
[development guide](../docs/development.md#build-and-flash).

Using the owner's authorization header, post the URL-encoded form
`mode=link-test&seconds=6` to `/api/action` while the authenticated
status sampler is running. The opt-in handler accepts 5–45 seconds,
acknowledges the request, then deliberately disconnects only the
controller's Wi-Fi. Compare uptime and fresh observations after it
reassociates. Repeat with longer intervals and an open browser. Restore the
default firmware with the regular `app-flash` afterward. The default
build rejects this action.

A fresh simulator process starts a new synthetic meter. Reusing an
installation with saved meter anchors can create an accounting gap.
Use **Activity → Electricity accounting** to review a gap and resume from
fresh readings when appropriate.
