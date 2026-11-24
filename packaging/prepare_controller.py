#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Build a controller package from a committed source snapshot."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import ssl
import subprocess
import tarfile
import tempfile
import tomllib
from prepare_network_update import RELEASE_VERSION, manifest as sign_ota_manifest

ROOT = Path(__file__).resolve().parents[1]
IMAGE_PATHS = {
    "0x0": "bootloader/bootloader.bin",
    "0x8000": "partition_table/partition-table.bin",
    "0xf000": "ota_data_initial.bin",
    "0x20000": "dispatch01.bin",
}
LINKED_MEMBERS = re.compile(r"^\s*0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+?\.a)\(([^\n]+?)\)", re.M)
GITHUB_RELEASES = "https://github.com/Spectre-Works/dispatch/releases"
CERTIFICATE = r"-----BEGIN CERTIFICATE-----\r?\n[A-Za-z0-9+/=\r\n]+-----END CERTIFICATE-----"


def command(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def digest(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def git_snapshot(revision, snapshot, directory):
    # Only tracked files from the selected commit enter the snapshot.
    with snapshot.open("wb") as output:
        with gzip.GzipFile(filename="", fileobj=output, mode="wb", mtime=0) as compressed:
            with subprocess.Popen(
                ["git", "archive", "--format=tar", revision], cwd=ROOT, stdout=subprocess.PIPE
            ) as archive:
                shutil.copyfileobj(archive.stdout, compressed)
                if archive.wait():
                    raise ValueError("Could not archive the selected revision")
    directory.mkdir()
    with tarfile.open(snapshot) as source:
        source.extractall(directory, filter="data")


def linked_archives(map_file):
    text = map_file.read_text()
    _, found, text = text.partition("Linker script and memory map\n")
    if not found:
        raise ValueError(f"Missing live linker sections in {map_file.name}")
    text = text.partition("Cross Reference Table\n")[0]
    archives = set()
    for address, size, archive, member in LINKED_MEMBERS.findall(text):
        if int(address, 16) and int(size, 16):
            if member == "x509_crt_bundle.S.obj":
                raise ValueError("The optional Mozilla CA bundle was linked")
            archives.add(Path(archive).name)
    if "libmain.a" not in archives:
        raise ValueError(f"Could not identify linked archives in {map_file.name}")
    return sorted(archives)


def collect_notices(scope, source, sdk, toolchain, controller, bundle, scope_file):
    files = [source / "LICENSE", source / "LICENSING.md", source / "THIRD_PARTY_NOTICES.md",
             source / "REUSE.toml", scope_file]
    files += sorted((source / "LICENSES").glob("*.txt"))
    files += sorted((source / "legal/terms").glob("*.md"))
    for path in files:
        destination = bundle / path.relative_to(source)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)

    locations = {
        "sdk_notices": sdk,
        "toolchain_notices": toolchain / "share" / "licenses",
        "managed_notices": controller / "managed_components",
    }
    for group, origin in locations.items():
        for name, path in scope[group].items():
            destination = bundle / "third-party" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(origin / path, destination)

    for name, (path, end) in scope["sdk_notice_prefixes"].items():
        notice, separator, _ = (sdk / path).read_text().partition(end)
        if not separator or len(notice) > 4096:
            raise ValueError(f"Review the notice boundary in {path}")
        destination = bundle / "third-party" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(notice.rstrip() + "\n")


def prepare_ota_trust(bundle, ca, signing_key):
    pem = ca.read_text(encoding="ascii")
    if not re.fullmatch(rf"(?:\s*{CERTIFICATE}\s*)+", pem):
        raise ValueError("The OTA CA file must contain only PEM certificates")
    ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT).load_verify_locations(cadata=pem)
    ca_path = bundle / "trust/ota-ca.pem"
    signer_path = bundle / "trust/ota-signer-public.pem"
    ca_path.parent.mkdir()
    ca_path.write_text(pem, encoding="ascii")
    signer_path.write_bytes(subprocess.check_output(["openssl", "pkey", "-in", signing_key, "-pubout"]))
    return ca_path, signer_path


def prepare_ota_assets(bundle, build, trust, signing_key, release_base):
    image_path = bundle / "firmware/dispatch01.bin"
    image = image_path.read_bytes()
    ca, signer = trust
    if any(path.read_bytes().rstrip(b"\n") not in image for path in trust):
        raise ValueError("The release image does not embed the supplied OTA trust inputs")
    config = (bundle / "sdkconfig").read_text()
    if "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\n" not in config:
        raise ValueError("The release bootloader must support rollback")
    ota = bundle / "ota"
    ota.mkdir()
    shutil.copyfile(image_path, ota / "dispatch01.bin")
    signed = sign_ota_manifest(image_path, signing_key)
    payload, signature, _ = signed.rsplit(b"\n", 2)
    signature_file = build / "ota-signature.der"
    signature_file.write_bytes(bytes.fromhex(signature.decode("ascii")))
    subprocess.run(
        ["openssl", "dgst", "-sha256", "-verify", signer, "-signature", signature_file],
        input=payload + b"\n", stdout=subprocess.DEVNULL, check=True,
    )
    (ota / "dispatch01-ota-v1.txt").write_bytes(signed)
    return {
        "release_base": release_base, "image": "ota/dispatch01.bin",
        "manifest": "ota/dispatch01-ota-v1.txt",
        "ca_sha256": digest(ca), "signing_public_key_sha256": digest(signer),
    }


def prepare(revision, output, release_version=None, ota_ca=None, signing_key=None, release_base=GITHUB_RELEASES):
    if output.exists():
        raise ValueError("Choose a new output directory")
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise ValueError("Activate the ESP-IDF environment before building")
    sdk = Path(idf_path).resolve()
    sha = command("git", "rev-parse", "--verify", "--end-of-options", f"{revision}^{{commit}}")
    epoch = command("git", "show", "-s", "--format=%ct", sha)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".dispatch-rehearsal-", dir=output.parent) as temp:
        work = Path(temp)
        bundle, source, build = (work / name for name in ("bundle", "source", "build"))
        bundle.mkdir(mode=0o700)
        git_snapshot(sha, bundle / "source.tar.gz", source)
        scope_file = source / "legal/license-scope.json"
        if not scope_file.is_file():
            scope_file = source / "legal/status.json"
        license_id = json.loads(scope_file.read_text())["operative_application_identifier"]
        if not re.fullmatch(r"LicenseRef-[A-Za-z0-9.-]+", license_id):
            raise ValueError("Unexpected application license identifier")
        if not (source / "LICENSES" / f"{license_id}.txt").is_file():
            raise ValueError("The source revision is missing its application license")
        scope = tomllib.loads((source / "packaging/controller-inputs.toml").read_text())
        sdk_revision = command("git", "-C", sdk, "rev-parse", "HEAD")
        if sdk_revision != scope["sdk_revision"] or command("git", "-C", sdk, "status", "--porcelain", "-uno"):
            raise ValueError("Use the pinned, unmodified ESP-IDF checkout")
        compiler_path = shutil.which("xtensa-esp32s3-elf-gcc")
        if not compiler_path:
            raise ValueError("The pinned ESP32-S3 compiler is unavailable")
        compiler = Path(compiler_path).resolve()
        compiler_version = command(compiler, "--version").splitlines()[0]
        if scope["gcc_version"] not in compiler_version:
            raise ValueError("The compiler does not match the pinned ESP-IDF toolchain")

        controller = source / "apps/controller"
        lock_digest = digest(controller / "dependencies.lock")
        version = release_version or sha[:12]
        env = dict(os.environ, SOURCE_DATE_EPOCH=epoch)
        flags = [f"-DPROJECT_VER={version}"]
        if release_version:
            trust = prepare_ota_trust(bundle, ota_ca, signing_key)
            ca, signer = trust
            flags.extend([f"-DDISPATCH_OTA_CA={ca}", f"-DDISPATCH_OTA_SIGNER={signer}",
                          f"-DDISPATCH_OTA_RELEASE_BASE={release_base}",
                          "-DDISPATCH_LINK_TEST=OFF", "-DDISPATCH_OTA_TRIAL_FAIL=OFF"])
        subprocess.run(["idf.py", "-C", controller, "-B", build, *flags, "build"], env=env, check=True)
        description = json.loads((build / "project_description.json").read_text())
        expected = {"project_name": "dispatch01", "target": "esp32s3", "project_version": version}
        if any(description[key] != value for key, value in expected.items()):
            raise ValueError("Firmware build identity does not match the source snapshot")
        if digest(controller / "dependencies.lock") != lock_digest:
            raise ValueError("The build changed the committed dependency lock")
        # The selected SDK also owns the loader in the full installation image.
        inventory = {
            name: linked_archives(build / path)
            for name, path in (("application", "dispatch01.map"), ("bootloader", "bootloader/bootloader.map"))
        }
        linked = set(inventory["application"]) | set(inventory["bootloader"])
        allowed = set(scope["linked_archives"])
        if linked != allowed:
            raise ValueError(f"Review linked archive changes. Added {sorted(linked - allowed)}. Removed {sorted(allowed - linked)}.")
        flash = json.loads((build / "flasher_args.json").read_text())
        if flash["flash_files"] != IMAGE_PATHS or flash["flash_settings"]["flash_size"] != "8MB":
            raise ValueError("The controller flash layout changed")

        for path in IMAGE_PATHS.values():
            destination = bundle / "firmware" / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(build / path, destination)
        (bundle / "firmware/flash_args").write_text(
            " ".join(flash["write_flash_args"]) + "\n" +
            "".join(f"{offset} {path}\n" for offset, path in IMAGE_PATHS.items())
        )
        shutil.copyfile(controller / "sdkconfig", bundle / "sdkconfig")
        collect_notices(scope, source, sdk, compiler.parent.parent, controller, bundle, scope_file)

        manifest = {
            "source_commit": sha,
            "firmware_version": version,
            "target": "esp32s3",
            "license": license_id,
            "sdk_revision": sdk_revision,
            "sdk_version": scope["sdk_version"],
            "compiler": compiler_version,
            "dependency_lock_sha256": lock_digest,
            "linked_archives_by_image": inventory,
            "flash": {"settings": flash["flash_settings"], "files": IMAGE_PATHS},
        }
        if release_version:
            manifest["network_update"] = prepare_ota_assets(bundle, build, trust, signing_key, release_base)
        (bundle / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        checksums = (f"{digest(path)}  {path.relative_to(bundle)}\n" for path in sorted(bundle.rglob("*")) if path.is_file())
        (bundle / "SHA256SUMS").write_text("".join(checksums))
        if output.exists():
            raise ValueError("Output appeared while preparing the rehearsal")
        bundle.rename(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="New local directory for the package")
    parser.add_argument("--revision", default="HEAD", help="Git commit or ref to build")
    parser.add_argument("--release-version", help="Build USB and signed OTA assets for this vMAJOR.MINOR.PATCH")
    parser.add_argument("--ota-ca", type=Path, help="PEM trust roots for the release and download hosts")
    parser.add_argument("--signing-key", type=Path, help="Private ECDSA P-256 release signing key")
    parser.add_argument("--ota-release-base", help="Override the GitHub release base for a bench rehearsal")
    args = parser.parse_args()
    requested = (args.release_version, args.ota_ca, args.signing_key)
    if (any(requested) or args.ota_release_base) and not all(requested):
        parser.error("Supply --release-version, --ota-ca, and --signing-key together")
    if args.release_version and not RELEASE_VERSION.fullmatch(args.release_version):
        parser.error("Use vMAJOR.MINOR.PATCH with up to six digits per component")
    try:
        prepare(args.revision, args.output.absolute(), args.release_version,
                args.ota_ca, args.signing_key, args.ota_release_base or GITHUB_RELEASES)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Controller packaging failed: {exc}\n")
    print(f"Controller package ready at {args.output}")


if __name__ == "__main__":
    main()
