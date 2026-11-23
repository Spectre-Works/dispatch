<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Set up external equipment

Use the controller on a trusted LAN. Pair only outlets rated for the actual
load, including startup inrush. Confirm the physical outlet mapping in
**Equipment → First-run checks** before enabling automation. Assign one
automation owner to each switch or operating setpoint. Disable competing
rules in other controllers for equipment that DISPATCH will operate.

The [compatibility matrix](compatibility.md) distinguishes hardware
observations from simulator and protocol fixture results.

## Pair an outlet

In **Settings → Equipment & plan → Pair equipment**, enter the outlet address
and the ID below. Add the miner's separate IPv4 address to observe its cgminer
API. Save the association, confirm its mapping, and review the operating plan.

| Connection | Outlet address | Device ID or switch entity | Credential |
| --- | --- | --- | --- |
| Shelly original generation | Device IPv4 | Full lowercase local ID, such as `shellyswitch25-a1b2c3@1`. For devices whose `/shelly` page has `type` and `mac`, use the device family ID and lowercase MAC suffix. Use six digits in short-ID mode and twelve in long-ID mode. | `username:password` if Basic authentication is enabled. |
| Shelly RPC | Device IPv4 | Full ID from `/shelly`. An omitted channel means 0. Use `@1` for the second relay or the documented add-on ID for a Pro 3EM output add-on. | Local device password if SHA-256 Digest is enabled. |
| Tasmota HTTP | Device IPv4 | `tasmota-020809aabbcc@1` with the device's twelve lowercase MAC digits. Use the configured relay number 1–8. | Tasmota WebPassword if set. |
| Tasmota MQTT | Broker IPv4 | `mqtt-020809aabbcc-MinerRack` with the device MAC and its case-sensitive MQTT Topic. This profile uses one relay. | Set in **Energy → MQTT broker connection**. |
| Home Assistant | Home Assistant IPv4 | The exact `switch.miner` entity. | Set in **Energy → Home Assistant connection and entities**. |

Shelly and Tasmota provide meter values where the attached device and selected
profile expose them. The timed first-run test needs a Shelly output with a
working power and cumulative energy meter and a device-managed flip-back
timer. For other outputs, enter a startup estimate under **Energy → Solar
estimate and equipment power settings**. Spending limits need cumulative
outlet energy. Grid flow alone cannot supply the missing outlet counter.

## Set up Home Assistant

Save the private LAN address, HTTP port, and a long-lived access token for
an account that can read and operate the selected entities. Store the token
only in the controller setup and rotate it in Home Assistant if exposed.
This connection uses HTTP on the trusted equipment LAN. The controller sends
the token only to the saved address and port.

- Pair `switch.*` entities as outlets. In **Meter sensors for a Home Assistant
  outlet**, optionally select a per-outlet power sensor in W, kW, or mW and a
  cumulative energy sensor in Wh, kWh, or MWh. Select a counter with
  `total` or `total_increasing` state class. Use sensors that measure that
  outlet alone.
- In **Connect or verify a whole-site meter**, select a power sensor reporting
  positive grid import and negative export. Entity availability and units
  must be valid before DISPATCH uses a reading. Configure an availability
  rule at the source if the integration retains a cached reading after loss
  of equipment contact.
- The optional battery and temperature fields display charge percentage and
  temperature. They do not act as grid power observations.
- Select one `number.*`, `select.*`, or restart-class `button.*` control.
  Apply a value offered by the entity's range, step, or option list. DISPATCH
  sends the domain's service call and checks the resulting entity state. For
  a button it checks for a changed press timestamp. Use controls for routine
  equipment operation and keep installer, protection and calibration entities
  outside this mapping.

Home Assistant can bridge supported Zigbee and Z-Wave devices. Set up those
radios and equipment in Home Assistant first. If a request is unconfirmed,
check the device and entity state before submitting another action.

## Install an integration profile

Choose a stock example from the [DISPATCH SDK v0.1.0](https://github.com/Spectre-Works/dispatch-sdk/tree/v0.1.0/profiles)
and edit the fields for your device. Run the SDK validator, then upload the JSON
or packed file in **Settings → Integration profiles**. Each controller saves
one mapping per family. The upload applies to every device in that family
after the controller restarts.

Equipment, meter, and Home Assistant setup forms offer **Fill from SDK
template**. Review the selected entities before saving the form.
Pair devices and enter credentials in the usual setup forms.

Changes to operating mappings open Preview at the next boot. Verify fresh
outlet and meter readings before resuming. A change to the active grid
mapping clears grid direction verification. Compare import and export
against a known reading and verify direction again. To go back to the
factory mapping, choose **Restore stock** and restart. Keep your custom
profile files with your configuration backups.

## Connect a LAN bridge

Run a [DISPATCH SDK bridge](https://github.com/Spectre-Works/dispatch-sdk/blob/v0.2.0/bridge.md)
on a separate LAN machine to add a device protocol or run your own integration
code. Enter the bridge address, port, and token as a Home Assistant REST
connection, then select the bridge's entities. This supports metered outlets,
grid power, battery and temperature sensors, and the configured number,
select, or restart action.

A bridge that provides production readings can also be selected under
**Energy → Connect a solar production source**. Choose **DISPATCH SDK
bridge**, use the saved REST address, and enter the 12-digit gateway ID
reported by the bridge. DISPATCH waits for matching, fresh production and
the same evidence of sustained output used for a native ECU-R. A production
reading estimates available headroom after your configured reserve.

To put a device under bridge control, replace its pairing with the switch
entity in the same equipment slot. Check the new binding in Preview
before resuming. Assign write control for that device to one controller.

## Set up the MQTT broker

Use an existing broker with an authenticated account, a unique Tasmota topic
of up to 21 characters, and ACLs that bind responses to the intended device.
Use the default Tasmota `cmnd/%topic%/` and `stat/%topic%/` paths and one
relay. Select a clean Tasmota session with `SetOption140 0` and plain relay
feedback with `SetOption90 0`. Disable offline QoS 0 queueing at the broker.
Require that all clients keep control commands non-retained. Retire any older retained command on the
selected device's control paths before commissioning.

For TLS, enter the broker's private IPv4 address, port, certificate DNS
name, and one CA certificate in PEM format. The base64url-encoded certificate
must fit within 1600 characters. The controller checks the issuer, name, and
validity period. Check the controller clock and the broker's certificate chain
if the connection fails. Plain MQTT over TCP is available for a trusted
isolated LAN with a blank TLS name and certificate.

Use **Check device status** with the MQTT device ID to verify the broker
connection, MAC and relay reading before pairing. The controller opens a
short clean session and requests fresh Tasmota status
to identify the device and observe the relay. It sends explicit ON or OFF
at QoS 0 with retain disabled. A later poll confirms the observed state.
A broker connection, publish, or immediate reply is insufficient to confirm
switching.

## Connect a native SunSpec or Solarman meter

Open **Energy → Connect or verify a whole-site meter**. Select **SunSpec
Modbus TCP** or **Solarman V5** and enter the endpoint and meter settings.
**Load meter profile** fills the form from a
[DISPATCH SDK native profile](https://github.com/Spectre-Works/dispatch-sdk/blob/main/native-grid.md).
Save once with direction unchecked. After comparing readings with known
physical import and export, save with direction verified.

- **SunSpec Modbus TCP** uses the Common model serial, configured unit,
  zero-based SunSpec signature address, meter model, and meter position.
  Models 201–204 return scaled integer watts. Models 211–214 return float
  watts. For a SolarEdge inverter with an attached import/export meter,
  use its local Modbus TCP port, inverter serial, and the attached grid
  meter's model and position.
- **Solarman V5** uses the logger serial and device unit. Enter the exact
  inverter's zero-based signed 16-bit grid power register, read function
  (03 or 04), and decimal scale exponent. Both clocks must be synchronized
  for DISPATCH to accept the observation timestamp.

Select the raw meter polarity so DISPATCH reports positive import and
negative export. These drivers supply grid measurements through the Atom's
LAN connection. Use the connection details of the actual grid meter or
logger. Changes to native meter settings require verifying direction again.

## Verify grid flow and set an ESS target

In **Energy → Connect or verify a whole-site meter**, enter the meter address
and the corresponding ID. Save it with verification cleared, then compare
with an independent import and export measurement. Positive watts must mean
import and negative watts must mean export. Verify direction only after those
checks. DISPATCH then uses the source for surplus decisions.

- **Shelly:** Use the full local device ID. Pro 3EM and original 3EM use all
  three phases. Pro EM can use the CT on channel 0 or the whole-site CT on
  channel 1 with `@1`. Use the relay's independent outlet status for switching
  when such an output exists.
- **Fronius:** Use `fronius-solar-api` with the private LAN Solar API v1 address.
  Enable the API on the inverter or Datamanager first. A grid-side Smart
  Meter and normal grid-connected operation are required for grid authority.
  The Energy page displays reported solar production separately. Hybrid
  inverters can report DC production. That value is informational.
- **Victron:** Use `victron-1p`, `victron-2p`, or `victron-3p` for the phasing
  of the whole-site GX meter. Requires Venus OS 3.70 or later, system unit
  100, TCP 502, a stable address, and an active grid input. Enable Modbus TCP
  in the GX settings. Leave it read-only for telemetry alone. Modbus TCP has
  no protocol authentication. Limit network access to the intended controller.

For compatible Victron ESS installations, enable **Read/write** Modbus access
and use **Energy → Victron ESS grid target**. The GX must be in ESS Mode 2
with Dynamic ESS off and the grid meter verified. Enter an integer target
within both the installation's limits and the controller's ±10,000 W range.
Positive requests import and negative requests export. This is a volatile
setpoint. Actual grid flow can differ because of inverter, battery and site
limits. A zero readback can mean zero or an unset override. Confirm the result
on the GX before repeating an unconfirmed request. DISPATCH sends the request
once and does not restore it after reboot.

[GX Modbus TCP manual](https://www.victronenergy.com/live/ccgx:modbustcp_faq) ·
[Victron ESS control documentation](https://www.victronenergy.com/live/ess:ess_mode_2_and_3)

## Restart or change a miner preset

Use **Equipment → Power mode** for the [documented Goldshell profiles](../integrations/miners/README.md).
Native restart on supported firmware requires a fresh powered outlet and
miner reading, an allowed operating period, and the dashboard password.
A new mining process is confirmed when the miner's reported elapsed counter
restarts. An acknowledged request without that evidence remains unconfirmed.
Preset changes use observed factory settings and active board clocks, with
the miner password supplied for manual commands or managed authorization.
