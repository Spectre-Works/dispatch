#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Serve synthetic Home Assistant, Tasmota HTTP, and Tasmota MQTT endpoints."""

import argparse
import ipaddress
import json
import re
import socketserver
import ssl
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

HA_TOKEN = "fixture.home-token"
TASMOTA_PASSWORD = "fixture-pwd"
DEVICE_MAC = "02:08:09:AA:BB:CC"
MQTT_USER = "fixtureuser"
MQTT_PASSWORD = "fixturepass"
TOPIC = "fixture"


class Relay:
    def __init__(self, watts):
        self.watts = watts
        self.on = False
        self.wh = 10.0
        self.at = time.monotonic()

    def sample(self):
        now = time.monotonic()
        if self.on:
            self.wh += self.watts * (now - self.at) / 3600
        self.at = now
        return self.on, self.watts if self.on else 0, self.wh

    def set(self, on):
        self.sample()
        self.on = on


class Equipment:
    def __init__(self, drop_ack):
        self.lock = threading.Lock()
        self.ha = Relay(250)
        self.tasmota = Relay(420)
        self.reserve, self.preference, self.restart = 35, "Default", "unknown"
        self.drop_ack = drop_ack

    def tasmota_status(self, kind):
        on, watts, wh = self.tasmota.sample()
        status = {
            "5": {"StatusNET": {"Mac": DEVICE_MAC}},
            "10": {"StatusSNS": {"ENERGY": {"Power": watts, "Total": round(wh / 1000, 6)}}},
            "11": {"StatusSTS": {"POWER": "ON" if on else "OFF"}},
        }
        return {key: value for section in status.values() for key, value in section.items()} if kind == "0" else status[kind]

    def ha_state(self, entity):
        on, watts, wh = self.ha.sample()
        value, attributes = {
            "switch.fixture": ("on" if on else "off", {}),
            "sensor.fixture_power": (watts, {"unit_of_measurement": "W", "device_class": "power"}),
            "sensor.fixture_energy": (round(wh, 3), {"unit_of_measurement": "Wh", "device_class": "energy",
                                                   "state_class": "total_increasing"}),
            "sensor.fixture_grid": (watts - 1000, {"unit_of_measurement": "W", "device_class": "power"}),
            "sensor.fixture_battery": (72, {"unit_of_measurement": "%", "device_class": "battery"}),
            "sensor.fixture_temperature": (24, {"unit_of_measurement": "°C", "device_class": "temperature"}),
            "number.fixture_reserve": (self.reserve, {"min": 0, "max": 100, "step": 1}),
            "select.fixture_preference": (self.preference, {"options": ["Default", "Surplus"]}),
            "button.fixture_restart": (self.restart, {"device_class": "restart"}),
        }[entity]
        return {"entity_id": entity, "state": str(value), "attributes": attributes}

    def ha_action(self, service, args):
        entity = args["entity_id"]
        if entity == "switch.fixture" and service in ("switch/turn_on", "switch/turn_off"):
            self.ha.set(service == "switch/turn_on")
        elif entity == "number.fixture_reserve" and service == "number/set_value":
            value = args["value"]
            if type(value) not in (int, float) or value not in range(101):
                raise ValueError("invalid reserve")
            self.reserve = value
        elif entity == "select.fixture_preference" and service == "select/select_option":
            option = args["option"]
            if option not in ("Default", "Surplus"):
                raise ValueError("invalid option")
            self.preference = option
        elif entity == "button.fixture_restart" and service == "button/press":
            self.restart = datetime.now(timezone.utc).isoformat()
        else:
            raise ValueError("invalid action")


class HTTP(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, code, value):
        body = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        equipment = self.server.equipment
        url = urlsplit(self.path)
        if self.server.tasmota:
            fields = parse_qs(url.query)
            if url.path != "/cm":
                self.reply(404, {})
                return
            if fields.get("user") != ["admin"] or fields.get("password") != [TASMOTA_PASSWORD]:
                self.reply(401, {})
                return
            command = fields.get("cmnd", [""])[0]
            with equipment.lock:
                if command == "Status 0":
                    status = equipment.tasmota_status("0")
                elif command in ("Power1 On", "Power1 Off"):
                    equipment.tasmota.set(command.endswith("On"))
                    if equipment.drop_ack:
                        return
                    status = {"POWER": "ON" if equipment.tasmota.on else "OFF"}
                else:
                    status = {"Command": "Unknown"}
            self.reply(200, status)
            return
        if self.headers.get("Authorization") != f"Bearer {HA_TOKEN}":
            self.reply(401, {})
            return
        entity = url.path.removeprefix("/api/states/")
        try:
            with equipment.lock:
                state = equipment.ha_state(entity)
            self.reply(200, state)
        except KeyError:
            self.reply(404, {})

    def do_POST(self):
        if self.server.tasmota or not self.path.startswith("/api/services/"):
            self.reply(404, {})
            return
        if self.headers.get("Authorization") != f"Bearer {HA_TOKEN}":
            self.reply(401, {})
            return
        equipment = self.server.equipment
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 512:
                raise ValueError("invalid body size")
            args = json.loads(self.rfile.read(size))
            with equipment.lock:
                equipment.ha_action(self.path.removeprefix("/api/services/"), args)
        except (KeyError, ValueError, TypeError):
            self.reply(400, {})
            return
        if not equipment.drop_ack:
            self.reply(200, [])


def mqtt_field(text):
    return len(text).to_bytes(2, "big") + text


def take_field(data):
    size = int.from_bytes(data[:2], "big")
    if len(data) < 2 or size > len(data) - 2:
        raise ValueError("invalid MQTT field")
    return data[2:2 + size], data[2 + size:]


class MQTT(socketserver.StreamRequestHandler):
    def setup(self):
        self.request.settimeout(2)
        if self.server.tls:
            self.request = self.server.tls.wrap_socket(self.request, server_side=True)
        super().setup()

    def finish(self):
        try:
            super().finish()
        finally:
            if self.server.tls:
                self.request.close()

    def frame(self):
        head = self.rfile.read(2)
        if len(head) != 2:
            raise EOFError()
        size = head[1] & 127
        if head[1] & 128:
            extra = self.rfile.read(1)
            if len(extra) != 1 or not 0 < extra[0] < 128:
                raise ValueError("invalid MQTT length")
            size += 128 * extra[0]
        if size > 512:
            raise ValueError("MQTT request exceeds fixture capacity")
        body = self.rfile.read(size)
        if len(body) != size:
            raise EOFError()
        return head[0], body

    def respond(self, kind, data):
        size = len(data)
        length = bytes([size]) if size < 128 else bytes([128 | (size & 127), size >> 7])
        self.wfile.write(bytes([kind]) + length + data)

    def handle(self):
        try:
            kind, data = self.frame()
            if kind != 0x10 or data[:8] != b"\x00\x04MQTT\x04\xc2":
                return
            client, data = take_field(data[10:])
            user, data = take_field(data)
            password, tail = take_field(data)
            authorized = client and user == MQTT_USER.encode() and password == MQTT_PASSWORD.encode() and not tail
            self.respond(0x20, b"\x00\x00" if authorized else b"\x00\x05")
            if not authorized:
                return
            kind, data = self.frame()
            if kind != 0x82 or len(data) < 2:
                return
            identifier, grants, topics = data[:2], bytearray(), set()
            data = data[2:]
            while data and len(grants) < 5:
                topic, data = take_field(data)
                if not data or data[0] != 0 or not re.fullmatch(rb"stat/fixture/(STATUS5|STATUS10|STATUS11|POWER1?)", topic):
                    return
                topics.add(topic)
                grants.append(0)
                data = data[1:]
            if data:
                return
            self.respond(0x90, identifier + grants)
            equipment = self.server.equipment
            for _ in range(4):
                kind, data = self.frame()
                path, value = take_field(data)
                if kind != 0x30:
                    return
                with equipment.lock:
                    if path == b"cmnd/fixture/Status" and value in (b"5", b"10", b"11"):
                        topic = b"stat/fixture/STATUS" + value
                        response = json.dumps(equipment.tasmota_status(value.decode())).encode()
                    elif path == b"cmnd/fixture/Power1" and value in (b"ON", b"OFF"):
                        equipment.tasmota.set(value == b"ON")
                        if equipment.drop_ack:
                            return
                        topic, response = b"stat/fixture/POWER", value
                    else:
                        return
                if topic in topics:
                    self.respond(0x30, mqtt_field(topic) + response)
        except (EOFError, OSError, ValueError):
            return


class MQTTServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, address):
        # A negative TLS check can close before application traffic starts.
        if not isinstance(sys.exception(), (ssl.SSLError, TimeoutError)):
            super().handle_error(request, address)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", required=True, type=ipaddress.IPv4Address)
    parser.add_argument("--ha-port", type=int, default=8123)
    parser.add_argument("--tasmota-port", type=int, default=80)
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--mqtt-cert", help="PEM broker certificate for the synthetic TLS endpoint")
    parser.add_argument("--mqtt-key", help="PEM broker private key for the synthetic TLS endpoint")
    parser.add_argument("--drop-control-ack", action="store_true")
    args = parser.parse_args()
    if not args.bind.is_private or args.bind.is_unspecified:
        parser.error("Use an explicit LAN address or 127.0.0.1.")
    if bool(args.mqtt_cert) != bool(args.mqtt_key) or (args.mqtt_cert and not args.mqtt_port):
        parser.error("Supply the synthetic certificate and key together with an enabled MQTT port.")
    tls = None
    if args.mqtt_cert:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(args.mqtt_cert, args.mqtt_key)
    host = str(args.bind)
    equipment = Equipment(args.drop_control_ack)
    servers = []
    try:
        for port, server_class, handler, tasmota in (
            (args.ha_port, ThreadingHTTPServer, HTTP, False),
            (args.tasmota_port, ThreadingHTTPServer, HTTP, True),
            (args.mqtt_port, MQTTServer, MQTT, False),
        ):
            if port == 0:
                continue
            server = server_class((host, port), handler)
            server.equipment, server.tasmota = equipment, tasmota
            if server_class is MQTTServer:
                server.tls = tls
            servers.append(server)
            threading.Thread(target=server.serve_forever, daemon=True).start()
        print(f"Fixture bound to {host}; HA {args.ha_port}, Tasmota HTTP {args.tasmota_port}, MQTT {args.mqtt_port}",
              flush=True)
        print(f"HA token {HA_TOKEN}; Tasmota identity tasmota-020809aabbcc@1 password {TASMOTA_PASSWORD}; "
              f"MQTT identity mqtt-020809aabbcc-{TOPIC} with {MQTT_USER}/{MQTT_PASSWORD}.", flush=True)
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
