#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Serve signed test releases over HTTPS on a reachable development LAN."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import socket
import ssl
import time

MANIFEST = "dispatch01-ota-v1.txt"
IMAGE = "dispatch01.bin"


class UpdateFixture(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            tag, mode = (self.server.releases / "selected").read_text().split()
            if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
                raise ValueError("Invalid release version")
            if mode not in ("good", "badsig", "badhash", "truncated"):
                raise ValueError("Invalid response mode")
        except (OSError, ValueError):
            self.send_error(503, "Set selected to a release tag and response mode")
            return

        release_path = f"/releases/download/{tag}/"
        if self.path == f"/releases/latest/download/{MANIFEST}":
            self.send_response(302)
            self.send_header("Location", self.server.origin + release_path + MANIFEST)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if self.path not in (release_path + MANIFEST, release_path + IMAGE):
            self.send_error(404)
            return

        name = self.path.rsplit("/", 1)[1]
        try:
            data = (self.server.releases / tag / name).read_bytes()
        except OSError:
            self.send_error(404)
            return

        if mode == "badsig" and name == MANIFEST:
            replacement = b"a" if data[-2:-1] != b"a" else b"b"
            data = data[:-2] + replacement + b"\n"
        if mode == "badhash" and name == IMAGE:
            data = data[:4096] + bytes([data[4096] ^ 1]) + data[4097:]

        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            for offset in range(0, len(data), 4096):
                self.wfile.write(data[offset:offset + 4096])
                if name == IMAGE:
                    if mode == "truncated" and offset >= 65536:
                        self.connection.shutdown(socket.SHUT_RDWR)
                        return
                    time.sleep(0.025)
        except (ConnectionError, ssl.SSLError):
            self.log_message("Client disconnected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", required=True, help="Reachable development IPv4 address")
    parser.add_argument("--port", type=int, default=8443)
    parser.add_argument("--releases", type=Path, required=True)
    parser.add_argument("--cert", type=Path, required=True)
    parser.add_argument("--key", type=Path, required=True)
    args = parser.parse_args()

    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(args.cert, args.key)
    with ThreadingHTTPServer((args.bind, args.port), UpdateFixture) as server:
        server.origin = f"https://{args.bind}:{args.port}"
        server.releases = args.releases
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
