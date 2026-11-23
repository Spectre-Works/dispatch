#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0

"""Synthetic Shelly Gen2 RPC, cgminer summary, and SNTP for a development LAN."""

import argparse
import hashlib
import hmac
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import secrets
import socketserver
import struct
import threading
import time

PASSWORD = "synthetic-shelly-digest-fixture"
IDENTITY_PREFIX = "shellyplusplugsus-"
NTP_EPOCH = 2208988800


def sha(value):
    return hashlib.sha256(value.encode()).hexdigest()


class Equipment:
    def __init__(self, ip, index, controls):
        self.ip = ip
        self.identity = f"{IDENTITY_PREFIX}{index + 1:012x}"
        self.controls = controls
        self.is_on = False
        self.watts = 500 + index * 50
        self.energy = 1000.0
        self.last_updated = time.monotonic()
        self.on_since = None
        self.last_reset = None
        self.accepted = 0
        self.share_check = 0
        self.nonces = {}
        self.lock = threading.Lock()

    def options(self):
        if self.controls is None:
            return {}
        try:
            return json.loads(self.controls.read_text())
        except (OSError, ValueError):
            return {}

    def update(self, options):
        now = time.monotonic()
        if self.is_on:
            self.energy += self.watts * (now - self.last_updated) / 3600
        reset = options.get("meter_reset")
        if reset and reset != self.last_reset:
            self.energy = 0
            self.last_reset = reset
        self.last_updated = now
        if self.on_since is not None and now - self.on_since >= 7:
            self.accepted += max(0, int((now - max(self.share_check, self.on_since + 7)) / 2))
            if self.accepted == 0 or now - self.share_check >= 2:
                self.share_check = now
        return now

    def nonce(self):
        now = time.monotonic()
        self.nonces = {nonce: until for nonce, until in self.nonces.items() if until > now}
        nonce = secrets.token_urlsafe(18)
        self.nonces[nonce] = now + 10
        return nonce

    def authenticate(self, header):
        if not header.startswith("Digest "):
            return False
        attrs = {
            name: quoted or bare
            for name, quoted, bare in re.findall(r'(\w+)=(?:"([^"]+)"|([^, ]+))', header[7:])
        }
        nonce, cnonce = attrs.get("nonce", ""), attrs.get("cnonce", "")
        until = self.nonces.pop(nonce, 0)
        if not until or time.monotonic() >= until:
            return False
        have = attrs.get("response", "")
        expected = sha(f"{sha(f'admin:{self.identity}:{PASSWORD}')}:{nonce}:00000001:{cnonce}:auth:{sha('POST:/rpc')}")
        required = {
            "username": "admin", "realm": self.identity, "uri": "/rpc",
            "algorithm": "SHA-256", "qop": "auth", "nc": "00000001",
        }
        return cnonce and all(attrs.get(k) == v for k, v in required.items()) and hmac.compare_digest(expected, have)


class Shelly(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def send(self, status, payload=None, challenge=None):
        data = b"" if payload is None else json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Connection", "close")
        self.send_header("Content-Length", str(len(data)))
        if data:
            self.send_header("Content-Type", "application/json")
        if challenge:
            self.send_header("WWW-Authenticate", challenge)
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        self.connection.settimeout(2)
        self.close_connection = True
        eq = self.server.equipment
        options = eq.options()
        if options.get("shelly_down"):
            return
        length = int(self.headers.get("Content-Length", "0"))
        if self.path != "/rpc" or not 0 < length <= 256:
            self.send(400)
            return
        try:
            request = json.loads(self.rfile.read(length))
        except ValueError:
            self.send(400)
            return
        with eq.lock:
            now = eq.update(options)
            if not options.get("no_digest") and not eq.authenticate(self.headers.get("Authorization", "")):
                nonce = eq.nonce()
                self.send(401, challenge=f'Digest qop="auth", realm="{eq.identity}", nonce="{nonce}", algorithm=SHA-256')
                return
            params = request.get("params", {})
            method = request.get("method")
            if params.get("id") != 0:
                self.send(400)
                return
            if method == "Switch.GetStatus":
                result = {"id": 0, "output": eq.is_on, "apower": eq.watts if eq.is_on else 0,
                          "aenergy": {"total": round(eq.energy, 3)}}
            elif method == "Switch.Set" and isinstance(params.get("on"), bool):
                want = params["on"]
                was = eq.is_on
                print(f"SIMULATED relay {eq.identity} requested {'on' if want else 'off'}", flush=True)
                if options.get("reject_sets"):
                    self.send(503)
                    return
                if not options.get("unconfirmed_sets"):
                    eq.is_on = want
                    if was != eq.is_on:
                        eq.on_since = now if eq.is_on else None
                        eq.accepted = 0
                        eq.share_check = now
                result = {"was_on": was}
            else:
                self.send(400)
                return
            self.send(200, {"id": request.get("id"), "src": eq.identity, "result": result})


class Miner(socketserver.BaseRequestHandler):
    def handle(self):
        eq = self.server.equipment
        options = eq.options()
        if options.get("miner_down"):
            return
        self.request.settimeout(1.5)
        data = bytearray()
        while len(data) < 64 and not data.endswith(b"}"):
            part = self.request.recv(64 - len(data))
            if not part:
                return
            data.extend(part)
        if data != b'{"command":"summary"}':
            return
        with eq.lock:
            now = eq.update(options)
            if not eq.is_on:
                return
            active = now - eq.on_since >= 7
            status = {"STATUS": [{"STATUS": "S", "When": int(time.time()), "Code": 11, "Msg": "Summary",
                                  "Description": "SIMULATED cgminer summary"}],
                      "SUMMARY": [{"Elapsed": int(now - eq.on_since), "MHS 5s": 315.5 if active else 0.0,
                                   "Accepted": eq.accepted, "Rejected": 0}], "id": 1}
        self.request.sendall(json.dumps(status, separators=(",", ":")).encode() + b"\0")


class Ntp(socketserver.BaseRequestHandler):
    def handle(self):
        message, sock = self.request
        if len(message) < 48 or message[0] & 7 != 3:
            return

        def timestamp():
            now = time.time_ns()
            return struct.pack("!II", (now // 10**9 + NTP_EPOCH) & 0xFFFFFFFF,
                               ((now % 10**9) << 32) // 10**9)

        stamp = timestamp()
        reply = bytes((0x24, 2, 4, 0xEC)) + bytes(8) + b"LAB0" + stamp + message[40:48] + stamp + timestamp()
        sock.sendto(reply, self.client_address)


class MinerServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def start(server, equipment=None):
    server.equipment = equipment
    threading.Thread(target=server.serve_forever, daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ip", nargs="+", help="Explicit development-only addresses")
    parser.add_argument("--control-file", type=Path, help="Optional JSON fault controls")
    args = parser.parse_args()
    for i, ip in enumerate(args.ip):
        eq = Equipment(ip, i, args.control_file)
        start(ThreadingHTTPServer((ip, 80), Shelly), eq)
        start(MinerServer((ip, 4028), Miner), eq)
        print(f"SIMULATED {eq.identity} and cgminer on {ip}", flush=True)
    start(socketserver.UDPServer((args.ip[0], 123), Ntp))
    threading.Event().wait()


if __name__ == "__main__":
    main()
