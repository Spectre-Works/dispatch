<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->

# DISPATCH/01 compatibility

[Integration setup](integration-setup.md) describes configuration and commissioning.

| Interface | Target and connection | Observations and controls | Evidence |
| --- | --- | --- | --- |
| Shelly Gen1 | Native HTTP. First-generation Plug `SHPLG-1`/`SHPLG2-1`, Plug S `SHPLG-S`, Plug US `SHPLG-U1`, 1PM `SHSW-PM`, 2.5 `SHSW-25` in relay mode, and 3EM `SHEM-3`. Bind the device ID and selected channel. Local Basic auth when configured. | Relay state and explicit on/off, watts and energy on metered relay channels. 3EM sums valid signed phase power as a grid source and can operate its separate contactor. Measure miner consumption independently of the contactor. | Manufacturer Gen1 API, fixture evidence. [Gen1 API](https://shelly-api-docs.shelly.cloud/gen1/) |
| Shelly RPC outlets | Native HTTP on supported Switch components. Plus Plug US channel 0 and Pro 4PM channel 2 are fixture targets. Pair an exact source ID and channel. SHA-256 Digest when configured. | Output state, available metering, explicit on/off, and timed test on metered channels that support it. A Pro 3EM with the Pro Output Add-on can expose a separate Switch component in 100–199. | Prior live Plus Plug US operation, additional channel and output add-on fixtures. [Switch API](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/Switch/) |
| Shelly RPC energy | Native HTTP. Pro 3EM in three-phase mode with `EM:0`, or Pro EM with CT at the grid point on selected `EM1:0` or `EM1:1`. | Signed grid watts after physical direction commissioning. The Pro EM's independent `Switch:0` dry contact uses the outlet path. Base Pro 3EM without an output add-on is a measurement source. | Prior Pro 3EM live observations, Pro EM fixture. [Pro EM](https://shelly-api-docs.shelly.cloud/gen2/Devices/Gen2/ShellyProEM/) · [Pro 3EM](https://shelly-api-docs.shelly.cloud/gen2/Devices/Gen2/ShellyPro3EM/) · [EM1](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/EM1/) |
| Tasmota HTTP | Native HTTP on Wi-Fi relay builds with `Status 0`, stable station MAC, and indexed `PowerN` output from 1 to 8, or unindexed `Power` for output 1. Local web command interface, with optional `admin` and the configured WebPassword. | Explicit on/off plus subsequent status. For a single output with scalar ENERGY data, report power in W and cumulative energy in kWh. Per-channel energy and device-timed fail-off tests have no supported mapping. | Independent adapter and synthetic command/status fixtures. [Commands](https://tasmota.github.io/docs/Commands/) · [Status response fields](https://tasmota.github.io/docs/JSON-Status-Responses/) |
| Tasmota MQTT | Existing MQTT 3.1.1 broker. Single-relay Tasmota with identity `mqtt-<mac>-<case-sensitive-topic>`, default `%prefix%/%topic%/` layout, `ON`/`OFF` payloads, and broker credentials and ACL. Use the 12-digit lowercase Wi-Fi MAC and a 1–21 character topic containing letters, digits, underscores or dashes. TLS uses a matching hostname and one owner-supplied broker CA PEM, limited to 1,600 unpadded base64 characters. | One bounded socket using fresh `STATUS 5/11/10` replies for MAC, relay and optional measurements. Explicit on/off uses non-retained QoS 0 with device identity preflight and a later relay observation. Only fresh status confirms the relay. The adapter starts a clean session and uses no command queue across reconnection. | Synthetic broker fixtures and a controller TLS status probe with MAC and hostname rejection. [MQTT standard](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html) · [Tasmota MQTT](https://tasmota.github.io/docs/MQTT/) |
| Home Assistant REST | Existing Home Assistant bridge. Selected entity IDs over trusted LAN HTTP and a Home Assistant long-lived bearer token. Allow access only from the controller's trusted network. | Selected units and availability, explicit switch on/off, constrained number and select operations, and restart-class button presses. Actions call the corresponding service and inspect selected state afterward where state is observable. A button response indicates acknowledgment, while a changed button timestamp indicates that Home Assistant registered the press. HA automations and DISPATCH need one owner for each actuator. | Synthetic REST fixtures plus controller-to-simulator number, select and button checks with lost service acknowledgments. [REST API](https://contributors.home-assistant.io/core/internals/rest-api/) · [Number](https://www.home-assistant.io/integrations/number/) · [Select](https://www.home-assistant.io/integrations/select/) · [Button](https://www.home-assistant.io/integrations/button/) |
| Victron GX ESS | GX gateway such as Cerbo GX with Venus OS 3.70 or later, Modbus TCP enabled on LAN port 502, system unit 100. On the GX, enable read/write access for control. Commission the number of grid phases and direction against a known external reading. | Aggregate signed 32-bit grid watts from 908, 910 and 912, positive import, with AC source 826 reporting grid. Read and write the volatile grid target 2716–2717 as signed 32-bit W, high word first, with one two-register write. DISPATCH accepts targets from −10,000 to 10,000 W, subject to site limits. Before writing, check ESS configuration 2902, Dynamic ESS mode 5423, grid source and current target. Observe the target and measured watts separately. | Manufacturer protocol and pinned server mapping. Synthetic Modbus fixtures exercise the adapter. [GX manual](https://www.victronenergy.com/live/ccgx:modbustcp_faq) · [ESS mode 2](https://www.victronenergy.com/live/ess:ess_mode_2_and_3) · [32-bit power register introduction](https://github.com/victronenergy/dbus_modbustcp/commit/79736c14b12535a4a363aa4cac0af8fae14ba4e4) |
| Fronius Solar API V1 | Native LAN HTTP on compatible Fronius inverter or Datamanager 2.0. Vendor Power Flow endpoint requires hybrid firmware 1.2.1-X or later or non-hybrid 3.3.9-X or later. Enable the Solar API where required on GEN24, Tauro and Verto. | Power Flow supplies signed grid power for a commissioned grid-point meter outside backup mode. DISPATCH uses eligible grid readings for the operating plan and shows reported PV as status. The public Solar API is a measurement interface. | Vendor document 42,0410,2012,EN rev. 021-15052025, sections 2.4 and 4.11, and synthetic fixtures. [Solar API V1 manual](https://www.fronius.com/~/downloads/Solar%20Energy/Operating%20Instructions/42,0410,2012.pdf) |
| Goldshell native restart | Native local HTTP. Stock KD BOX 2.2.4, HS BOX 2.2.6 and HS Lite 2.2.5 only, as identified by their status response. Use the miner login and bearer session token. | One explicit bodyless `PUT /mcb/restart` per request. Acknowledgment and a reset of the mining process's reported elapsed time provide distinct evidence. Existing Idle and factory preset coverage have independent model and firmware requirements. | Vendor UI documentation, stock firmware archives and synthetic adapter fixtures. Physical restart remains unverified. [BOX guide](https://goldshellhelp.zendesk.com/hc/en-us/articles/17608608063513-BOX-Series-Tutorial) · [Pinned firmware index](https://github.com/goldshellminer/firmware/blob/14eed80815ab264c820d3a06a7c5fba8da63047b/README.md) · [Miner fixtures](../tests/goldshell_mode_test.c) |

Victron GX can return zero for an unavailable mapped numeric value. An
all-zero grid sample cannot establish meter direction or verify that it is
current. Compare with both import and export under known conditions during
commissioning. Keep Dynamic ESS and any competing setpoint controller
disabled when exercising the DISPATCH control. Site equipment and export
limits still apply to the owner's chosen target. A zero target readback may
also represent an unavailable override. Treat the grid measurement as
separate evidence. [Pinned server response
encoding](https://github.com/victronenergy/dbus_modbustcp/blob/205bf75ecfc4e6263a3d36db5b5c1ee19f38f74d/mappings.cpp#L260)

## Protocol provenance

The adapters use independent implementations. See the
[third-party inventory](../THIRD_PARTY_NOTICES.md) for linked dependencies.

| Source | Use in DISPATCH |
| --- | --- |
| [Tasmota 6d1a2e43](https://github.com/arendst/Tasmota/tree/6d1a2e430e2e15eeb69b35dca45ce2422bb56dde) | GPL-3.0 firmware code excluded. The local adapter uses published web command and status protocol facts. |
| [Home Assistant Core 09cac8e9](https://github.com/home-assistant/core/tree/09cac8e90f239898f6447b6ff59c3a8abb1065fe) and [aioshelly d770cd5e](https://github.com/home-assistant-libs/aioshelly/tree/d770cd5e636b71a8dff978a563ae1dd8e6e78c38) | Each has its own Apache-2.0 license. Core's REST implementation and aioshelly's Python package remain external. Selected entity and Shelly actions use public API documentation. |
| [ESP-IDF v6.1.1 / 051e4c4](https://github.com/espressif/esp-idf/tree/051e4c4eb20346e043cdd47bc388fde75ff8af63) | Unmodified Apache-2.0 ESP-TLS API and the SDK's existing Mbed TLS support. Excluded `components/mbedtls/esp_crt_bundle/cacrt_all.pem` from the linked application image because its upstream Mozilla NSS certificate data, converted to PEM by curl, is MPL-2.0. [ESP-IDF certificate source documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_crt_bundle.html) · [curl license and provenance](https://curl.se/docs/caextract.html) |
| [Victron GX server v1.0.104 / 205bf75e](https://github.com/victronenergy/dbus_modbustcp/tree/205bf75ecfc4e6263a3d36db5b5c1ee19f38f74d) | MIT `LICENSE.md`. Checked `attributes.csv` and `mappings.cpp` against the [manufacturer workbook](https://www.victronenergy.com/upload/documents/CCGX-Modbus-TCP-register-list-3.71.xlsx). The download on the assessment date had SHA-256 `5a81fbf8226bd1f35cfa681a438322766633f958d5a55a65a60ee2eff888110d` and internal revision 256. Protocol fields and encoding informed an independent implementation. No workbook or server files were incorporated. |
| [Goldshell stock firmware 14eed808](https://github.com/goldshellminer/firmware/tree/14eed80815ab264c820d3a06a7c5fba8da63047b) and [asic-rs 481edf4c](https://github.com/256foundation/asic-rs/tree/481edf4cdab1aaddf96e0fc21e3430dab673c975) | Manufacturer firmware archives have no verified redistribution grant for vendor UI/application files. They remain external. The Apache-2.0 asic-rs backend and its fixtures also remain external. Only verified interface behavior informed the DISPATCH restart adapter, as detailed below. |
| [cgminer b8491c66](https://github.com/ckolivas/cgminer/blob/b8491c66e7e22f23a9edf095dd1337ee581e88bd/API-README) | The existing cgminer RPC client recognizes the documented `Elapsed` field in `SUMMARY`. No GPL program code or documentation text was incorporated. |

### Reproduce the Goldshell protocol check

Use the exact archive names from the pinned manufacturer firmware index.
Record the complete archive hash before opening it. The CPB is an outer
archive containing an IUS image. On these three firmware archives, the
IUS contains an ext4 data volume at byte offset `0x95c600`, length
`0x2000000`. Read the volume without mounting or running firmware.
Confirm the model in `/root/minername` and version in `/root/version`.
Inspect the System dashboard's restart operation and authenticated
request declaration under `/root/yottamc/js/`. Keep extracted files in
temporary storage. The stock operation is distinct from factory reset.

| Archive | SHA-256 |
| --- | --- |
| `KDA-Miner/Goldshell-KDBox/firmware-Goldshell-KDBox-2.2.4.cpb` | `ed5a09cd7894d1bbff2b4c36a59dccfdfce606e2f084a2146fea4db8688b877f` |
| `Handshake-&-Siacoin-Miner/Goldshell-HSBox/firmware-Goldshell-HSBox-2.2.6.cpb` | `a3077cf37ecfb1caa68b35d9296381f2231f92bc1a19225e579dabaadea5f70c` |
| `Handshake-&-Siacoin-Miner/Goldshell-HSLITE/firmware-Goldshell-HSLITE-2.2.5.cpb` | `c90b7128c126f2f4b45676c17566bf255b4dae2d2cd0af43d45081a8ac3d8246` |

Run the [host checks](development.md#checks-and-simulation) to exercise the
production adapters with synthetic endpoints.
