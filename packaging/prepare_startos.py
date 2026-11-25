#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Build and verify signed StartOS Preview packages from a Git revision."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
ARCHES = ("x86_64", "aarch64")
PACKAGE_ID = "dispatch-preview"
LICENSE_ID = "LicenseRef-Spectre-Dispatch-1.0"
CHECKSUMS = "DISPATCH-STARTOS-SHA256SUMS"
RELEASE = re.compile(r"v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", re.ASCII)
ED25519_PUBLIC_PREFIX = bytes.fromhex("302a300506032b6570032100")


def run(*args, cwd=None, env=None, capture=False):
    return subprocess.run(args, cwd=cwd, env=env, check=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verified_download(url, checksum, target):
    run("curl", "--fail", "--silent", "--show-error", "--location",
        "--retry", "3", "--connect-timeout", "15", "--max-time", "90", "--output", target, url)
    if digest(target) != checksum:
        raise ValueError(f"Input checksum failed for {target.name}")


def snapshot(revision, work):
    commit = run("git", "rev-parse", "--verify", "--end-of-options", f"{revision}^{{commit}}",
                 cwd=ROOT, capture=True).decode().strip()
    archive = work / "source.tar"
    with archive.open("wb") as output:
        subprocess.run(("git", "archive", "--format=tar", commit), cwd=ROOT, stdout=output, check=True)
    source = work / "source"
    source.mkdir()
    # The archive contains only the selected repository tree.
    with tarfile.open(archive) as tar:
        tar.extractall(source, filter="data")
    return source, commit


def get_cli(config, work):
    machine = platform.machine()
    if machine not in ARCHES or platform.system() != "Linux":
        raise ValueError("Run the package builder on x86_64 or aarch64 Linux")
    name = f"start-cli_{machine}-linux"
    url = f"{config['repository']}/releases/download/start-cli/v{config['version']}/{name}"
    installed = shutil.which("start-cli")
    if installed and digest(Path(installed)) == config[machine]:
        return Path(installed)
    cli = work / "start-cli"
    verified_download(url, config[machine], cli)
    cli.chmod(0o700)
    return cli


def sdk_notices(package, notices):
    installed = json.loads((package / "package-lock.json").read_text())["packages"]
    records = []
    sdk_version = installed["node_modules/@start9labs/start-sdk"]["version"]
    for location in sorted(installed):
        if not location:
            continue
        module = package / location
        meta = json.loads((module / "package.json").read_text())
        version = meta.get("version")
        if not version and installed[location].get("inBundle"):
            version = f"bundled-with-start-sdk-{sdk_version}"
        if not version:
            raise ValueError(f"Missing version metadata for SDK dependency {location}")
        identifier = f"{meta['name']}@{version}"
        records.append(f"{identifier}  {meta.get('license', 'See package notice')}")
        for notice in sorted(module.iterdir()):
            if notice.is_file() and re.match(r"^(licen[cs]e|copying|notice)([.-]|$)", notice.name, re.I):
                shutil.copyfile(notice, notices / f"{identifier.replace('/', '_')}-{notice.name}")
    (notices / "npm-inventory.txt").write_text("\n".join(records) + "\n")


def collect_inputs(source, package, assets, config, commit, tag):
    parser = source / ".startos-inputs" / "http-parser"
    parser.mkdir(parents=True)
    for filename in ("http_parser.c", "http_parser.h", "LICENSE.txt"):
        entry = config["http_parser"]
        verified_download(f"{entry['base']}/{filename}", entry[filename], parser / filename)
    assets.mkdir()
    shutil.copyfile(source / "LICENSES" / f"{LICENSE_ID}.txt", package / "LICENSE.md")
    shutil.copytree(source / "LICENSES", assets / "LICENSES")
    shutil.copytree(source / "legal" / "terms", assets / "terms")
    for name in ("LICENSE", "LICENSING.md", "THIRD_PARTY_NOTICES.md"):
        shutil.copyfile(source / name, assets / name)
    notices = assets / "upstream"
    notices.mkdir()
    shutil.copyfile(parser / "LICENSE.txt", notices / "http-parser.LICENSE.txt")
    for filename, entry in config["notices"].items():
        verified_download(entry["url"], entry["sha256"], notices / filename)
    shutil.copyfile(package / "inputs.toml", assets / "inputs.toml")
    (assets / "SOURCE.json").write_text(json.dumps({"release": tag, "source_commit": commit}, indent=2) + "\n")
    sdk_notices(package, notices)


def verify_package(cli, package, path, arch, version, public_key, work):
    with path.open("rb") as stream:
        if stream.read(3) != b";;\x02" or stream.read(32) != public_key:
            raise ValueError(f"{path.name} has an unexpected S9PK version or signer")
    info = json.loads(run(cli, "s9pk", "inspect", path, "manifest", cwd=package, capture=True))
    if (info.get("id") != PACKAGE_ID or info.get("version") != version
            or info.get("osVersion") != "0.4.2"
            or info.get("hardwareRequirements", {}).get("arch") != [arch]
            or info.get("images", {}).get("dispatch", {}).get("arch") != [arch]):
        raise ValueError(f"{path.name} has an unexpected version, minimum OS, or architecture")
    # Start CLI deserializes the signed v2 header and checks every Merkle file
    # while serializing. Run it on a temporary copy to preserve the built bytes.
    verify_copy = work / "verify.s9pk"
    shutil.copyfile(path, verify_copy)
    try:
        run(cli, "s9pk", "convert", verify_copy, cwd=package)
    finally:
        verify_copy.unlink(missing_ok=True)


def prepare(revision, tag, output, signing_key, expected_signer):
    if output.exists():
        raise ValueError("Choose a new output directory")
    key = signing_key.resolve(strict=True)
    if key.is_relative_to(ROOT) or not key.is_file() or key.stat().st_mode & 0o077:
        raise ValueError("Provide an owner-only StartOS private key outside the checkout")
    public = run("openssl", "pkey", "-in", key, "-passin", "pass:",
                 "-pubout", "-outform", "DER", capture=True)
    if len(public) != len(ED25519_PUBLIC_PREFIX) + 32 or not public.startswith(ED25519_PUBLIC_PREFIX):
        raise ValueError("StartOS package signing requires an Ed25519 private key")
    fingerprint = hashlib.sha256(public).hexdigest()
    if expected_signer and expected_signer != fingerprint:
        raise ValueError("StartOS package key does not match the configured release signer")
    version = f"{tag[1:]}:0"
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dispatch-startos-") as workspace, \
         tempfile.TemporaryDirectory(prefix=".startos-release-", dir=output.parent) as staging:
        work = Path(workspace)
        artifacts = Path(staging) / "artifacts"
        artifacts.mkdir()
        source, commit = snapshot(revision, work)
        package = source / "packaging" / "startos"
        config = tomllib.loads((package / "inputs.toml").read_text())
        cli = get_cli(config["start_cli"], work)
        settings = work / ".startos"
        settings.mkdir(mode=0o700)
        (settings / "build.key.pem").symlink_to(key)
        index = package / "startos" / "index.ts"
        text = index.read_text()
        placeholder = "version: '1.0.0:0',"
        if text.count(placeholder) != 1:
            raise ValueError("StartOS package version placeholder is missing")
        index.write_text(text.replace(placeholder, f"version: '{version}',"))
        run("npm", "ci", "--ignore-scripts", "--no-audit", "--no-fund", cwd=package)
        assets = package / "assets"
        collect_inputs(source, package, assets, config, commit, tag)
        run(package / "node_modules/.bin/tsc", "--noEmit", cwd=package)
        run(package / "node_modules/.bin/ncc", "build", "startos/index.ts", "-o", "javascript", cwd=package)
        env = dict(os.environ, DISPATCH_PACKAGE_RELEASE_VERSION=tag)
        for arch in ARCHES:
            path = artifacts / f"{PACKAGE_ID}-{tag}-{arch}.s9pk"
            run(cli, "s9pk", "pack", ".", "--arch", arch, "--output", path, cwd=package, env=env)
            verify_package(cli, package, path, arch, version, public[len(ED25519_PUBLIC_PREFIX):], work)
        (artifacts / "STARTOS-SOURCE.json").write_text(json.dumps(
            {"release": tag, "source_commit": commit, "signer_sha256": fingerprint}, indent=2) + "\n")
        files = sorted(artifacts.iterdir())
        (artifacts / CHECKSUMS).write_text("".join(f"{digest(path)}  {path.name}\n" for path in files))
        run("sha256sum", "-c", CHECKSUMS, cwd=artifacts)
        artifacts.rename(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--release-version", required=True)
    parser.add_argument("--signing-key", type=Path, required=True)
    parser.add_argument("--expected-signer-sha256", help="SHA-256 of the expected DER public key")
    args = parser.parse_args()
    if not RELEASE.fullmatch(args.release_version):
        parser.error("Use a vMAJOR.MINOR.PATCH release version")
    if args.expected_signer_sha256 and not re.fullmatch(r"[0-9a-f]{64}", args.expected_signer_sha256):
        parser.error("The expected signer must be a lowercase SHA-256 fingerprint")
    try:
        prepare(args.revision, args.release_version, args.output.absolute(),
                args.signing_key, args.expected_signer_sha256)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"StartOS package build failed: {error}\n")


if __name__ == "__main__":
    main()
