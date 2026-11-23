#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0

"""Sample DISPATCH/01 resource and observation status over a development LAN."""

import argparse
import ipaddress
import json
from pathlib import Path
import re
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.request import ProxyHandler, Request, build_opener


def observe(address, key, duration, interval):
    uri = f"http://{address}/api/status"
    opener = build_opener(ProxyHandler({}))
    started = time.monotonic()
    count = outages = unhealthy = recoveries = 0
    offline = False
    lowest = {}
    begin_uptime = last_uptime = None
    limit = 65536
    while time.monotonic() - started < duration:
        now = time.monotonic()
        try:
            request = Request(uri, headers={"X-Dispatch-Key": key})
            with opener.open(request, timeout=4) as reply:
                body = reply.read(limit + 1)
            if len(body) > limit:
                raise ValueError("Status response exceeded sample size limit")
            status = json.loads(body)
            resources = status["resources"]
            for name in ("free", "minimum", "largest", "lowestBrowserLargest", "webStack", "workerStack"):
                value = resources[name]
                if value:
                    lowest[name] = min(lowest.get(name, value), value)
            count += 1
            if offline:
                recoveries += 1
                offline = False
            if any(device["ip"] and device["outlet"] is None for device in status["devices"]):
                unhealthy += 1
            if begin_uptime is None:
                begin_uptime = resources["uptime"]
            last_uptime = resources["uptime"]
        except HTTPError as err:
            if err.code == 401:
                raise ValueError("Controller rejected the owner key") from None
            outages += 1
            offline = True
        except (URLError, TimeoutError, ConnectionError):
            outages += 1
            offline = True
        time.sleep(max(0, min(now + interval, started + duration) - time.monotonic()))
    return {
        "durationSeconds": round(time.monotonic() - started, 2),
        "intervalSeconds": interval,
        "statusSamples": count,
        "requestsUnavailable": outages,
        "requestRecoveries": recoveries,
        "samplesWithoutFreshOutlet": unhealthy,
        "firstUptime": begin_uptime,
        "lastUptime": last_uptime,
        "lowestResourceSamplesBytes": lowest,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address", type=ipaddress.IPv4Address)
    parser.add_argument("--key-file", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--interval", type=float, default=3)
    args = parser.parse_args()
    key = args.key_file.read_text().strip()
    if not re.fullmatch("[a-f0-9]{32}", key) or args.seconds < 1 or args.interval < 0.5:
        parser.error("Expect a file containing an owner key, a positive duration, and an interval of at least half a second.")
    result = observe(args.address, key, args.seconds, args.interval)
    print(json.dumps(result, indent=2))
    sys.exit(0 if result["statusSamples"] > 0 else 1)


if __name__ == "__main__":
    main()
