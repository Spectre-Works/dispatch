# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Exercise the real controller REST adapter against the public LAN bridge."""

from pathlib import Path
import subprocess
import sys
from threading import Thread


def main():
    sys.path.insert(0, str(Path(sys.argv[1]).resolve()))
    from dispatch_sdk.bridge import Bridge, Entity, Reading, SolarReading

    state = {"relay": False, "limit": 10, "mode": "High", "restart": "unknown"}
    entities = {
        "switch.demo": Entity(
            lambda: Reading("on" if state["relay"] else "off"),
            lambda value: state.update(relay=value),
        ),
        "sensor.power": Entity(lambda: Reading(
            "300" if state["relay"] else "0", {"unit_of_measurement": "W", "device_class": "power"}
        )),
        "sensor.energy": Entity(lambda: Reading(
            "1.5", {"unit_of_measurement": "kWh", "device_class": "energy", "state_class": "total"}
        )),
        "sensor.grid": Entity(lambda: Reading("-125", {"unit_of_measurement": "W", "device_class": "power"})),
        "number.limit": Entity(
            lambda: Reading(str(state["limit"]), {"min": 5, "max": 20, "step": 5}),
            lambda value: state.update(limit=value),
        ),
        "select.mode": Entity(
            lambda: Reading(state["mode"], {"options": ["High", "Low"]}),
            lambda value: state.update(mode=value),
        ),
        "button.restart": Entity(
            lambda: Reading(state["restart"], {"device_class": "restart"}),
            lambda _: state.update(restart="2026-11-23T00:00:00+00:00"),
        ),
    }
    ages = iter((3, 31))

    def solar():
        return SolarReading("123456789012", next(ages), 1300, 120, 5000, 2, 1)

    with Bridge(("127.0.0.1", 0), "fixture-sdk-bridge-token-0000000", entities, solar=solar) as bridge:
        worker = Thread(target=bridge.serve_forever)
        worker.start()
        try:
            subprocess.run([sys.argv[2], str(bridge.server_address[1])], check=True, timeout=10)
        finally:
            bridge.shutdown()
            worker.join()


if __name__ == "__main__":
    main()
