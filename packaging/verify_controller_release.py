#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Confirm the controller assets in a release draft came from its tagged build."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile


def read_member(archive, name, limit):
    for candidate in (name, "./" + name):
        try:
            member = archive.getmember(candidate)
            break
        except KeyError:
            continue
    else:
        raise ValueError(f"Controller bundle is missing {name}")
    if not member.isfile() or not 0 < member.size <= limit:
        raise ValueError(f"Controller bundle has an invalid {name}")
    with archive.extractfile(member) as stream:
        return stream.read()


def verify(directory, tag, commit):
    with tarfile.open(directory / f"dispatch01-usb-{tag}.tar.gz", "r:gz") as usb:
        manifest = json.loads(read_member(usb, "manifest.json", 64 * 1024))
        if manifest["source_commit"] != commit or manifest["firmware_version"] != tag:
            raise ValueError("The controller USB bundle must come from the release tag")
        image = read_member(usb, "firmware/dispatch01.bin", 0x340000)
        signed = read_member(usb, "ota/dispatch01-ota-v1.txt", 1024)
        signer = read_member(usb, "trust/ota-signer-public.pem", 1024)
    for filename, expected in (("dispatch01.bin", image), ("dispatch01-ota-v1.txt", signed)):
        if (directory / filename).read_bytes() != expected:
            raise ValueError(f"{filename} differs from the USB bundle")
    marker, version, length, sha, signature = signed.decode("ascii").splitlines()
    if (marker != "DISPATCH/01 OTA v1" or version != tag or length != str(len(image))
            or sha != hashlib.sha256(image).hexdigest()):
        raise ValueError("The signed firmware manifest does not describe the attached image")
    if manifest["network_update"]["signing_public_key_sha256"] != hashlib.sha256(signer).hexdigest():
        raise ValueError("The controller signer differs from the bundle manifest")
    with tempfile.TemporaryDirectory() as work:
        key = Path(work) / "signer.pem"
        sig = Path(work) / "signature.der"
        key.write_bytes(signer)
        sig.write_bytes(bytes.fromhex(signature))
        subprocess.run(("openssl", "dgst", "-sha256", "-verify", key, "-signature", sig),
                       input=signed.rsplit(b"\n", 2)[0] + b"\n", check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("tag")
    parser.add_argument("commit")
    args = parser.parse_args()
    try:
        verify(args.directory, args.tag, args.commit)
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Controller asset verification failed: {error}\n")


if __name__ == "__main__":
    main()
