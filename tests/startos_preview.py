#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Exercise StartOS Preview with isolated synthetic network services."""
import argparse
import base64
from contextlib import contextmanager, ExitStack
from http.server import ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import signal
import socketserver
import ssl
import struct
import subprocess
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

import bridge_simulator as bridge
import equipment_simulator as equipment

LAN = "10.241.0."
OUTLET, GRID, BRIDGE = (LAN + str(i) for i in range(1, 4))
IDENTITY = "shellyplusplugsus-000000000001"
MQTT_ID = "mqtt-020809aabbcc-fixture"
BASE = "http://127.0.0.1:18080"


class InstrumentedShelly(equipment.Shelly):
    def send(self, status, payload=None, challenge=None):
        result = (payload or {}).get("result", {})
        if "was_on" in result:
            self.server.equipment.writes += 1
        super().send(status, payload, challenge)


class Meter(equipment.Shelly):
    def do_GET(self):
        eq = self.server.equipment
        if eq.grid_lost:
            self.send(503)
        elif self.path == "/solar_api/v1/GetPowerFlowRealtimeData.fcgi":
            self.send(200, {"Head": {"Status": {"Code": 0}}, "Body": {"Data": {
                "Site": {"Mode": "bidirectional", "Meter_Location": "grid", "P_Grid": eq.grid, "P_PV": 1300}}}})
        elif self.path.startswith("/mcb/"):
            self.send(404)
        else:
            self.send(400)

    def do_POST(self):
        self.server.equipment.writes += 1
        self.send(403)


class Ecu(socketserver.StreamRequestHandler):
    def handle(self):
        request = self.rfile.readline(32)
        if request != b"APS1100160001END\n":
            self.server.equipment.writes += 1
            return
        firmware = b"ECU_R_1.2.3"
        size = 55 + len(firmware) + 3
        frame = bytearray(size)
        frame[:25] = f"APS11{size:04}0001212345678901".encode()
        struct.pack_into(">III", frame, 27, 300, 1300, 120)
        struct.pack_into(">HH", frame, 46, 1, 1)
        frame[52:] = f"{len(firmware):03}".encode() + firmware + b"END"
        self.wfile.write(frame)


class MonitorBridge(bridge.Equipment):
    def __init__(self):
        super().__init__(False)
        self.ha_writes = 0
        self.mqtt_writes = 0
        relay_set = self.tasmota.set

        def set_tasmota(state):
            self.mqtt_writes += 1
            relay_set(state)

        self.tasmota.set = set_tasmota

    def ha_action(self, service, args):
        self.ha_writes += 1
        super().ha_action(service, args)


@contextmanager
def service(binary, data):
    process = subprocess.Popen([binary, data, "18080"])
    try:
        wait_for(lambda: process.poll() is None and call("healthz", expected=(200, 503)) == "ready\n")
        yield (data / "owner.key").read_text()
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=10) == 0


def call(path, key=None, fields=None, expected=200):
    headers = {} if key is None else {"X-Dispatch-Key": key}
    data = None if fields is None else urlencode(fields).encode()
    request = Request(f"{BASE}/{path}", data, headers)
    try:
        result = urlopen(request, timeout=2)
    except HTTPError as error:
        result = error
    with result:
        body = result.read().decode()
        codes = expected if isinstance(expected, tuple) else (expected,)
        assert result.status in codes, (path, fields and fields.get("mode"), result.status, body)
        return json.loads(body) if result.headers.get_content_type() == "application/json" else body


def wait_for(check):
    until = time.monotonic() + 28
    while time.monotonic() < until:
        try:
            result = check()
            if result:
                return result
        except (URLError, ConnectionError):
            pass
        time.sleep(0.2)
    raise AssertionError("Timed out waiting for a fresh fixture observation")


def listener(stack, cls, addr, handler, eq):
    server = stack.enter_context(cls(addr, handler))
    equipment.start(server, eq)
    stack.callback(server.shutdown)
    return server


def broker_tls(directory):
    cert = directory / "broker.pem"
    key = directory / "broker.key"
    subprocess.run(("openssl", "req", "-new", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                    "-noenc", "-days", "1", "-subj", "/CN=broker.fixture",
                    "-addext", "subjectAltName=DNS:broker.fixture", "-keyout", key, "-out", cert),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    ca = base64.urlsafe_b64encode(ssl.PEM_cert_to_DER_cert(cert.read_text())).decode().rstrip("=")
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(cert, key)
    return ca, tls


def parity(build, directory):
    outputs = []
    for edition in ("atom", "startos"):
        replay = build / f"dispatch-{edition}-replay"
        data = directory / edition
        result = b"".join(subprocess.check_output((replay, data, stage)) for stage in ("replay", "restart"))
        result += b"".join(subprocess.check_output((replay, directory / f"sunset-{edition}", stage))
                           for stage in ("sunset", "sunset-restart"))
        result += subprocess.check_output((replay, directory / f"dropout-{edition}", "daylight-dropout"))
        outputs.append(result)
    assert outputs[0] == outputs[1], outputs
    print(outputs[0].decode(), end="", flush=True)


def exercise(build, directory, browser):
    parity(build, directory)
    eq = equipment.Equipment(OUTLET, 0, None)
    eq.is_on, eq.on_since, eq.writes = True, time.monotonic() - 30, 0
    meter = type("MeterState", (), {"grid": -850, "grid_lost": False, "writes": 0})()
    br = MonitorBridge()
    with ExitStack() as stack:
        listener(stack, ThreadingHTTPServer, (OUTLET, 80), InstrumentedShelly, eq)
        listener(stack, equipment.MinerServer, (OUTLET, 4028), equipment.Miner, eq)
        listener(stack, equipment.MinerServer, (OUTLET, 8899), Ecu, meter)
        listener(stack, ThreadingHTTPServer, (GRID, 80), Meter, meter)
        ha = listener(stack, ThreadingHTTPServer, (BRIDGE, 8123), bridge.HTTP, br)
        ha.tasmota = False
        tls = listener(stack, bridge.MQTTServer, (BRIDGE, 8883), bridge.MQTT, br)
        ca, tls.tls = broker_tls(directory)
        data = directory / "service"
        binary = build / "dispatch-preview"
        with service(binary, data) as owner:
            assert 'data-edition="startos"' in call("")
            call("api/status", expected=401)
            call("api/action", fields={"mode": "enable"}, expected=401)

            def status():
                return call("api/status", owner)

            def action(mode, http_status=200, **fields):
                return call("api/action", owner, {"mode": mode, **fields}, http_status)

            def pair(index, ip, identity, miner="", password=""):
                call("api/config", owner, dict(scope="pair", index=index, ip=ip, identity=identity,
                     miner=miner, auth="set" if password else "none", password=password))

            def mqtt(name):
                call("api/mqtt", owner, dict(ip=BRIDGE, port=8883, auth="set", name=name,
                     user=bridge.MQTT_USER, password=bridge.MQTT_PASSWORD, ca=ca))

            def probe(identity):
                action("mqtt-probe", identity=identity)
                wait_for(lambda: not status()["mqtt"]["probePending"])
                return status()["mqtt"]["probeResult"]

            def grid_status():
                return status()["energy"]["grid"]

            mqtt("mismatch.fixture")
            assert probe(MQTT_ID) != 0
            mqtt("broker.fixture")
            assert probe(MQTT_ID.replace("aabbcc", "000000")) == 4
            assert probe(MQTT_ID) == 0

            action("home-config", ip=BRIDGE, port=8123, auth="set", token=bridge.HA_TOKEN,
                   battery="sensor.fixture_battery", temperature="sensor.fixture_temperature",
                   actuator="number.fixture_reserve")
            pair(0, OUTLET, IDENTITY, OUTLET, equipment.PASSWORD)
            pair(1, BRIDGE, "switch.fixture")
            pair(2, BRIDGE, MQTT_ID)
            def observed():
                snapshot = status()
                if (all(d["outlet"] is not None for d in snapshot["devices"][:3])
                        and snapshot["devices"][0]["summary"] and snapshot["home"]["known"]):
                    return snapshot

            s = wait_for(observed)
            assert s["devices"][0]["outlet"] and s["devices"][0]["watts"] == 500
            assert s["devices"][2]["wh"] is not None
            assert s["home"]["value"] == 35
            device_id = s["devices"][0]["timelineId"]
            for index, d in enumerate(s["devices"][:3]):
                action("confirm-map", slot=index, ip=d["ip"], identity=d["identity"], miner=d["minerIp"])
            action("home-meter", slot=1, identity=s["devices"][1]["timelineId"],
                   power="sensor.fixture_power", energy="sensor.fixture_energy")
            action("solar-ecu", source="ecu", ip=OUTLET, id="212345678901", port=8899, nightStart=0, nightEnd=0)
            grid_fields = dict(ip=GRID, identity="fronius-solar-api", auth="none", password="")
            action("solar-meter", **grid_fields, verified=0)
            wait_for(lambda: grid_status()["fresh"])
            action("solar-meter", **grid_fields, verified=1)
            assert wait_for(lambda: status()["energy"]["solar"]["fresh"])
            assert grid_status()["watts"] == -850

            plan = dict(scope="plan", timezone="Etc/UTC", start=0, end=0, rate=12345, allowance=100000,
                        quiet=0, quietStart=0, quietEnd=0, rate1Enabled=0, rate1Start=0, rate1End=0, rate1Price=0,
                        rate2Enabled=0, rate2Start=0, rate2End=0, rate2Price=0, weekend=0, paidEnabled=1,
                        paidStart=0, paidEnd=0, weekendPaidStart=0, weekendPaidEnd=0, bridgeEnabled=0)
            call("api/config", owner, plan)
            for mode, fields in (
                ("enable", {}),
                ("power-test", {"slot": 0, "id": device_id, "start": 1, "confirm": 1}),
                ("power-test", {"slot": 0, "id": device_id, "start": 0}),
                ("goldshell-mode", {"slot": 0, "expected": 1, "desired": 2, "password": "fixture"}),
                ("miner-restart", {"slot": 0, "identity": device_id, "password": "fixture"}),
                ("preset-eval", {"slot": 0, "start": 1}),
                ("home-action", {"entity": "number.fixture_reserve", "value": 51, "option": ""}),
                ("gx-target", {"ip": GRID, "expected": 0, "watts": 180}),
                ("notice-test", {}),
                ("discover", {}),
            ):
                action(mode, http_status=403, **fields)
            action("pause", minutes=15)
            assert status()["paused"] and not status()["enabled"]
            action("resume")
            meter.grid = 450
            wait_for(lambda: grid_status()["watts"] == 450)
            meter.grid_lost = True
            wait_for(lambda: not grid_status()["fresh"])
            meter.grid_lost = False
            meter.grid = -560
            wait_for(lambda: grid_status()["fresh"] and grid_status()["watts"] == -560)
            def archived_events():
                return call("api/timeline?events=1", owner)["events"]

            old_event = "Owner cleared electricity accounting and daily reports"
            # A distinctive persisted event verifies that a clear reaches the archive.
            action("clear-account", confirm=1)
            wait_for(lambda: any(e["label"] == old_event for e in archived_events()))
            wait_for(lambda: call("api/reports", owner))
            before = status()
            assert before["history"]
            call("api/action", fields={"mode": "clear-activity", "confirm": 1}, expected=401)
            action("clear-activity", http_status=400)
            assert status()["activityGeneration"] == before["activityGeneration"]
            if browser:
                subprocess.run(("node", Path(__file__).with_name("startos_dashboard_test.cjs"),
                                BASE, data / "owner.key", directory / "export.json"), check=True)
            else:
                action("clear-activity", confirm=1)
            after = status()
            assert after["activityGeneration"] == before["activityGeneration"] + 1
            assert after["usedCents"] == 0 and all(e["text"] != old_event for e in after["history"])
            assert all(e["label"] != old_event for e in archived_events())
            for setting in ("allowance", "rate", "start", "end", "timezone", "enabled", "operating"):
                assert after[setting] == before[setting], setting
            for index in range(3):
                for setting in ("ip", "identity", "minerIp", "firstRun"):
                    assert after["devices"][index][setting] == before["devices"][index][setting], setting

        # Exercise the actual installation journal with old active flags and an
        # armed timed test. Fixture listeners remain in place across restart.
        subprocess.run((build / "dispatch-atom-replay", data, "arm-restored-command"), check=True)
        restored = directory / "restored"
        shutil.copytree(data, restored)
        with service(binary, restored) as restored_key:
            assert restored_key == owner
            s = wait_for(observed)
            assert s["planReady"] and s["rate"] == 12.345 and s["devices"][0]["firstRun"]["mapping"]
            assert s["activityGeneration"] == after["activityGeneration"]
            assert all(e["label"] != old_event for e in archived_events())
            assert not s["enabled"] and not s["devices"][0]["pending"] and s["devices"][0]["powerTest"]["phase"] == 0
        if browser:
            destination = directory / "transfer-destination"
            with service(binary, destination):
                subprocess.run(("node", Path(__file__).with_name("startos_dashboard_test.cjs"), BASE,
                                destination / "owner.key", directory / "export.json", "import"), check=True)
        assert eq.is_on and not br.ha.on and not br.tasmota.on and br.reserve == 35
        assert eq.writes == meter.writes == br.ha_writes == br.mqtt_writes == 0
    print("StartOS dashboard API, fixture observations, negative commands and restored active state passed.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--browser", action="store_true", help="also run the Playwright dashboard transfer check")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dispatch-preview-fixtures-") as temp:
        exercise(args.build.absolute(), Path(temp), args.browser)
