#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Sign the Network Update manifest for a versioned DISPATCH/01 app image."""

import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess


def manifest(image_path: Path, signing_key: Path) -> bytes:
    max_image = 0x340000
    with image_path.open("rb") as image_file:
        image = image_file.read(max_image + 1)
    # ESP image header (24 bytes), segment header (8), then esp_app_desc_t.
    if (not 288 <= len(image) <= max_image or image[0] != 0xE9
            or struct.unpack_from("<I", image, 32)[0] != 0xABCD5432
            or image[80:112].split(b"\0", 1)[0] != b"dispatch01"):
        raise ValueError("Expected a DISPATCH/01 app image that fits the 3.25 MiB OTA slot")
    version = image[48:80].split(b"\0", 1)[0]
    if not re.fullmatch(rb"v(0|[1-9][0-9]{0,5})(\.(0|[1-9][0-9]{0,5})){2}", version):
        raise ValueError("Build with a vMAJOR.MINOR.PATCH project version")
    key_info = subprocess.check_output(
        ["openssl", "pkey", "-in", signing_key, "-text_pub", "-noout"], text=True
    )
    if "ASN1 OID: prime256v1" not in key_info:
        raise ValueError("Expected an ECDSA P-256 signing key")
    signed = b"\n".join((
        b"DISPATCH/01 OTA v1", version, str(len(image)).encode(),
        hashlib.sha256(image).hexdigest().encode(), b"",
    ))
    signature = subprocess.check_output(
        ["openssl", "dgst", "-sha256", "-sign", signing_key], input=signed
    )
    return signed + signature.hex().encode() + b"\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--signing-key", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        data = manifest(args.image, args.signing_key)
        with args.output.open("xb") as output:
            output.write(data)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
