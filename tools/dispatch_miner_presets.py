#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Collect read-only Goldshell compatibility evidence from a specific miner."""

import argparse
import http.client
import ipaddress
import json
import re
import socket
import time


MAX_REPLY = 16_384
TIMEOUT = 3


def board_response(ip):
    with socket.create_connection((ip, 4028), timeout=TIMEOUT) as conn:
        conn.sendall(b'{"command":"devs"}')
        conn.shutdown(socket.SHUT_WR)
        deadline = time.monotonic() + TIMEOUT
        data = bytearray()
        while len(data) <= MAX_REPLY:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError
            conn.settimeout(remaining)
            chunk = conn.recv(min(4096, MAX_REPLY + 1 - len(data)))
            if not chunk or b"\0" in chunk:
                data.extend(chunk.split(b"\0", 1)[0])
                break
            data.extend(chunk)
        return bytes(data)


def http_response(ip, path):
    connection = http.client.HTTPConnection(ip, timeout=TIMEOUT)
    try:
        connection.request("GET", path, headers={"Connection": "close"})
        with connection.getresponse() as response:
            if response.status != 200:
                raise ValueError(f"HTTP {response.status}")
            return response.read(MAX_REPLY + 1)
    finally:
        connection.close()


def capture(fetch, summarize):
    try:
        body = fetch()
        if len(body) > MAX_REPLY:
            return {"error": "Response exceeded the capture limit"}
        record = json.loads(body)
        if not isinstance(record, dict):
            return {"error": "Expected a JSON object"}
        return {"responseBytes": len(body), **summarize(record)}
    except (OSError, http.client.HTTPException, ValueError, RecursionError):
        return {"error": "Endpoint returned no usable response"}


def text(value):
    if isinstance(value, str) and re.fullmatch(r"[\w .()+/-]{1,64}", value):
        return value
    return None


def number(value):
    return value if type(value) in (int, float) and 0 <= value <= 100_000 else None


def objects(value):
    return [item for item in value if isinstance(item, dict)] if isinstance(value, list) else []


def preset(option):
    info = option.get("info")
    clock = re.match(r"(\d{1,5}) MHz(?: |$)", info) if isinstance(info, str) else None
    return {"level": number(option.get("level")), "clockMHz": int(clock[1]) if clock else None}


def settings(record):
    plans = []
    for option in objects(record.get("powerplans")):
        if isinstance(option.get("mode"), list):
            plans.append({"algorithm": text(option.get("algo")), "presets": [preset(p) for p in objects(option["mode"])]})
        else:
            plans.append(preset(option))
    manual = record.get("manual")
    return {"schema": text(record.get("version")), "algorithm": text(record.get("algoname")),
            "manual": manual if isinstance(manual, bool) else None, "select": number(record.get("select")),
            "powerplans": plans}


def boards(record):
    return {"status": [text(row.get("STATUS")) for row in objects(record.get("STATUS"))],
            "boards": [{"id": number(board.get("ID")), "enabled": text(board.get("Enabled")),
                        "status": text(board.get("Status")), "clockMHz": number(board.get("clock"))}
                       for board in objects(record.get("DEVS"))]}


def algorithms(record):
    return {"schema": text(record.get("version")), "selectedId": number(record.get("algo_select")),
            "choices": [{"id": number(option.get("id")), "name": text(option.get("name"))}
                        for option in objects(record.get("algos"))]}


def web_boards(record):
    return {"status": number(record.get("status")),
            "boards": [{"id": number(board.get("id")), "minerStatus": number(board.get("minerstatus")),
                        "reportedPowerplan": number(board.get("powerplan"))}
                       for board in objects(record.get("data"))]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("miner_ip", type=ipaddress.IPv4Address)
    ip = str(parser.parse_args().miner_ip)
    result = {
        "identity": capture(lambda: http_response(ip, "/mcb/status"),
                            lambda record: {key: text(record.get(key)) for key in ("model", "firmware", "hardware")}),
        "settings": capture(lambda: http_response(ip, "/mcb/setting"), settings),
        "algorithms": capture(lambda: http_response(ip, "/mcb/algosetting"), algorithms),
        "devs": capture(lambda: board_response(ip), boards),
        "webDevs": capture(lambda: http_response(ip, "/mcb/cgminer?cgminercmd=devs"), web_boards),
    }
    print(json.dumps(result, indent=2, allow_nan=False))
    return int(any("error" in result[key] for key in ("identity", "settings", "devs")))


if __name__ == "__main__":
    raise SystemExit(main())
