#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0

"""Open a short-lived DISPATCH setup session using the physical USB cable."""

import argparse
import ipaddress
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
import time
from urllib.parse import urlencode

import serial


def owner_key(path):
    proposed = secrets.token_hex(16)
    if path is None:
        return proposed
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd) as existing:
            info = os.fstat(existing.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
                raise ValueError
            key = existing.read(40).strip()
        if not re.fullmatch("[a-f0-9]{32}", key):
            raise ValueError
        return key
    with os.fdopen(fd, "w") as saved:
        saved.write(proposed + "\n")
    return proposed


def print_qr(executable, text):
    subprocess.run([executable, "-t", "ANSIUTF8", "-o", "-"],
                   input=text.encode("ascii"), check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", help="Controller USB serial port")
    parser.add_argument("--owner-key-file", type=Path, help="Use or create an owner-only key file")
    parser.add_argument("--ntp", help="Prefill the NTP server in the phone setup QR")
    parser.add_argument("--dashboard", type=ipaddress.IPv4Address,
                        help="Print the owner sign-in QR for the controller's LAN address")
    args = parser.parse_args()
    if bool(args.port) == bool(args.dashboard) or (args.dashboard and not args.owner_key_file):
        parser.error("Supply the USB port for setup, or --dashboard with an existing --owner-key-file.")
    if args.ntp and not re.fullmatch("[a-zA-Z0-9.-]{1,63}", args.ntp):
        parser.error("Use an NTP hostname or IPv4 address.")
    if not sys.stdout.isatty():
        parser.exit(2, "Run this command in a private interactive terminal.\n")
    if args.dashboard and not args.owner_key_file.is_file():
        parser.exit(2, "Use the existing owner key file for the controller.\n")
    try:
        owner = owner_key(args.owner_key_file)
    except (OSError, ValueError):
        parser.exit(2, "Choose an owner-only file in a private directory for the permanent key.\n")
    encoder = shutil.which("qrencode")
    if args.dashboard:
        if not encoder:
            parser.exit(2, "qrencode is required for the dashboard QR.\n")
        print("Scan to open the controller dashboard on its LAN.", flush=True)
        print_qr(encoder, f"http://{args.dashboard}/#" + urlencode({"key": owner}))
        return
    key = f"{secrets.randbelow(10**12):012d}"
    port = serial.Serial(baudrate=115200, timeout=0.2, exclusive=True)
    port.port = args.port
    port.dtr = False
    port.rts = False
    with port:
        port.reset_input_buffer()
        port.write(f"\nDPAIR {key}\n".encode("ascii"))
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            line = port.readline()
            found = re.search(rb"Setup active on (DISPATCH-[0-9A-F]{6})", line)
            if found:
                ssid = found[1].decode("ascii")
                print(f"Join {ssid} within ten minutes.")
                print(f"Temporary Wi-Fi passphrase and setup key  {key}")
                print(f"Suggested separate permanent owner key  {owner}")
                if encoder:
                    print("\n1. Scan to join the temporary setup Wi-Fi.", flush=True)
                    print_qr(encoder, f"WIFI:T:WPA;S:{ssid};P:{key};;")
                    print("\n2. Scan to open setup with the fields filled in.", flush=True)
                    fragment = urlencode({"key": key, "owner": owner, "ntp": args.ntp or ""})
                    print_qr(encoder, "http://192.168.4.1/#" + fragment)
                else:
                    print("Open http://192.168.4.1/ and enter the temporary setup key.")
                return
    parser.exit(1, "Setup did not open. Reboot for a new session. For a configured unit, hold the front button during reboot.\n")


if __name__ == "__main__":
    main()
