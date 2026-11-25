#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Build the self-contained dashboard embedded by both editions."""

import argparse
import base64
import gzip
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("html", type=Path)
    parser.add_argument("compressed", type=Path)
    parser.add_argument("--startos", action="store_true")
    args = parser.parse_args()

    web = Path(__file__).resolve().parent
    icon_path = "../../assets/brand/dispatch-icon.svg"
    icon_url = b"data:image/svg+xml;base64," + base64.b64encode((web / icon_path).read_bytes())
    data = (web / "index.html").read_bytes().replace(icon_path.encode(), icon_url)
    if args.startos:
        data = data.replace(b'<html lang="en">', b'<html lang="en" data-edition="startos">', 1)
    args.html.write_bytes(data)
    args.compressed.write_bytes(gzip.compress(data, mtime=0))


if __name__ == "__main__":
    main()
