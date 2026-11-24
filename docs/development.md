<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Develop DISPATCH/01

Start with [AGENTS.md](../AGENTS.md) and [LICENSING.md](../LICENSING.md). For dashboard setup after Wi-Fi provisioning,
use the [operator guide](operations.md).

## Build and flash

Use **ESP-IDF v6.1.1** for the AtomS3-Lite. Activate your installed ESP-IDF
environment, then run these commands from the repository root.

```sh
source /path/to/esp-idf/export.sh
idf.py --version
idf.py -C apps/controller build
idf.py -C apps/controller size
```

On the existing development host, `~/.config/esp32-dev/activate.bash` is the
local launcher for the pinned SDK. The project entry point, managed component
lockfile, and default SDK configuration are under [apps/controller](../apps/controller/README.md).
The build embeds [the dashboard](../apps/web/index.html) in plain and gzipped
form. Editing it takes effect on the controller after an application build and
flash.

Identify the board by its stable USB path. On Linux, list
`/dev/serial/by-id/` and set `dispatch_port` to the corresponding device.
If the serial device is inaccessible or busy, check host serial permissions
and close any running monitor before continuing.

```sh
ls -l /dev/serial/by-id/
dispatch_port=/dev/serial/by-id/REPLACE_WITH_YOUR_CONTROLLER
```

For a board without the DISPATCH layout, record the detected flash size and
back up its readable flash to an owner-only directory outside the checkout.
These commands assume the verified 8 MiB target.

```sh
umask 077
python -m esptool --port "$dispatch_port" flash-id
python -m esptool --port "$dispatch_port" read-flash 0 0x800000 /private/path/original.bin
sha256sum /private/path/original.bin > /private/path/original.sha256
sha256sum -c /private/path/original.sha256
```

The initial full flash writes the bootloader, partition table, OTA metadata,
and first application slot. Use `app-flash` for subsequent development updates
with the same layout and slot. It preserves NVS and the existing partition
configuration. USB is the development and recovery interface. The second app
slot is reserved.

| Board state | Command |
| --- | --- |
| First DISPATCH flash or deliberate layout provisioning | `idf.py -C apps/controller -p "$dispatch_port" flash` |
| Routine firmware update | `idf.py -C apps/controller -p "$dispatch_port" app-flash` |
| Inspect boot over USB | `idf.py -C apps/controller -p "$dispatch_port" monitor` |

For the migration from the four-slot map to the eight-slot map, check that
the running application is in the first slot. Build the new application and
write only the partition table and first application over USB. The table
keeps both application offsets, NVS and the previous history offset. First
boot imports installation settings and reads the earlier Activity records.

```sh
idf.py -C apps/controller build
python -m esptool --chip esp32s3 --port "$dispatch_port" write-flash \
  0x8000 apps/controller/build/partition_table/partition-table.bin \
  0x20000 apps/controller/build/dispatch01.bin
```

Some serial monitors reset the controller on open. For an active installation,
use the LAN dashboard and status sampler to verify the build booted, retained
its expected settings, and continued to receive fresh readings.
[Hardware and partition map](../hardware/dispatch-01/README.md)

## Provision the controller

1. In a private interactive terminal with [pySerial](../tools/README.md#usb-pairing)
   available, run the USB pairing tool. Give the key file an owner-only
   location outside the repository. The parent directory must already exist.

   ```sh
   python3 tools/dispatch_pair.py "$dispatch_port" --owner-key-file /private/path/owner
   ```

2. Join the displayed **DISPATCH-…** Wi-Fi network and open
   `http://192.168.4.1/` within ten minutes. Unlock with the temporary setup
   key. Select the trusted 2.4 GHz LAN, enter its credentials, and enter the
   separate owner key. When `qrencode` is installed, the tool can show one QR
   to join setup Wi-Fi and another to open the setup form with the keys
   filled in. Use `--ntp` to prefill a preferred time server.
3. Save and reconnect your browser device to the selected LAN. Find the
   controller's DHCP address in the router or serial console and unlock the
   dashboard with the owner key. A DHCP reservation keeps its address stable.
   Use **Lock browser** to remove the saved key on a shared browser.

To reopen setup on a configured unit, hold the front user button during
power-up, then run the USB pairing command again. To generate a phone sign-in
QR on the LAN, run the tool with
`--dashboard CONTROLLER_IP --owner-key-file /private/path/owner`. It uses the
existing owner key and requires `qrencode`.

The setup network uses a temporary WPA2 passphrase. The station dashboard
uses HTTP with a separate owner key on a trusted LAN. Keep flash backups and
credentials outside version control. Physical flash access is part of the
trust boundary. [Security guidance](../SECURITY.md)

## Checks and simulation

Use Python 3.11+, a POSIX shell, and a host C compiler. From the repository root:

```sh
./tools/check.sh
```

With ESP-IDF v6.1.1 activated, add the protocol fixtures that link the SDK's
HTTP parser.

```sh
./tools/check.sh --with-idf
```

The default command runs the repository checker and the policy, evidence,
daylight, time-zone, notification, Goldshell, Shelly, Fronius, and Victron tests. Set `CC=clang` or pass
a compiler path to select a different host compiler. The script builds in a
temporary directory. Run `python3 tools/check_repository.py` by itself for
repository and document maintenance.

### Exercise the controller and browser

Run the [equipment simulator](../tests/README.md) on a development-only
address reachable by the controller. Pair the fixture outlet and miner, then
verify that Preview proposes the expected action. With the fixture selected
and control enabled, check that observed relay state follows the action and
fresh evidence returns after an induced fault. The simulator exercises
protocols and control policy. Physical equipment and load suitability have
separate evidence.

Set `DISPATCH_SDK_PATH` to a checkout of SDK tag `v0.2.0` when
checking the cross-repository contract. The host suite compares every
published stock profile against the compiled defaults. With `--with-idf`
it also runs the production REST adapter against the SDK bridge on loopback.

For dashboard changes, exercise the relevant workflow on a wide and a narrow
viewport. Where applicable, verify a successful save, the resulting Preview,
and what the UI shows for incomplete or stale readings. For firmware changes,
build and verify the full batch on the development controller as described in
[AGENTS.md](../AGENTS.md).

Use the authenticated status sampler or the periodic USB logs when checking
polling, browser load, or Wi-Fi recovery.

```sh
python3 tests/controller_observe.py CONTROLLER_IP \
  --key-file /private/path/owner --seconds 600 --interval 3
```

Record the workload, duration, firmware image size, minimum free internal
heap, largest free block, and task stack headroom. Use the
[Wi-Fi reassociation test](../tests/README.md#wi-fi-reassociation-test) for
an intentional link drop and restore the default build afterward.

## Optional LAN notifications

Open **Needs attention → Notifications**, enter a private IPv4 address, HTTP
port, and receiver path, and save. Use **Send test** to verify that the
receiver accepts a POST. The saved path is write-only in the dashboard.
The controller sends exceptions while the browser is closed.

The JSON payload contains `event`, `id`, and `slot`. Slots start at 1 and
a test uses a null slot. Event types are `work_missing`,
`switch_unconfirmed`, `destination_mismatch`, `recovery_exhausted`, and
`test`. Treat HTTP 2xx as acceptance. Deduplicate retries using the stable
`id` or `Idempotency-Key` header. Transient errors get up to three
attempts using the same ID. Delivery runs after equipment work with one
socket, a 350 ms attempt deadline, and a queue of eight. Pending deliveries
and counters clear on restart or when replacing the endpoint.

## Controller clock

**Settings → Controller clock** shows the last NTP sync and can retry it.
DISPATCH tries the preferred server, then Cloudflare and NIST unless
**Use only the preferred server** is enabled. It restarts NTP after a Wi-Fi
reconnect. Changing the clock source restarts the controller and preserves
its Wi-Fi and operating plan.

## Refresh embedded time-zone data

Verify the pinned IANA tzdata release and unpack it to a temporary directory.
With `tz_source` pointing to the extracted release and `tz_release` set to
its version, run the following from the repository root in Bash:

```sh
tz_compiled=$(mktemp -d)
zic -d "$tz_compiled" "$tz_source"/{africa,antarctica,asia,australasia,europe,northamerica,southamerica,etcetera,backward}
python3 tools/generate_controller_timezones.py "$tz_compiled" "$tz_release" > apps/controller/main/timezone_data.inc
rm -rf -- "$tz_compiled"
./tools/check.sh
```
