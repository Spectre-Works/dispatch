#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
"""Check license scopes, repository contents, and local Markdown links."""
from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
import hashlib
import json
from pathlib import Path
import re
import sys
import tomllib
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
APPLICATION = "LicenseRef-Spectre-Dispatch-1.0"
PROSE = "CC-BY-4.0"
TZDB = "LicenseRef-IANA-TZDB-Public-Domain"
REQUIRED = (
    "README.md", "START-HERE.md", "AGENTS.md", "LICENSE", "LICENSING.md",
    "CONTRIBUTING.md", "SECURITY.md", "THIRD_PARTY_NOTICES.md", "REUSE.toml",
    "docs/architecture.md", "docs/development.md", "docs/operating-policy.md",
    "docs/compatibility.md",
    "core/README.md", "integrations/README.md", "apps/controller/README.md",
    "apps/web/README.md", "hardware/dispatch-01/README.md",
    "legal/license-scope.json", "legal/reference/sources.json",
    f"LICENSES/{APPLICATION}.txt", f"LICENSES/{PROSE}.txt",
    "legal/terms/community-permissions-v1.0.md", "legal/terms/brand-policy-v1.0.md",
    "legal/terms/contributor-license-agreement-v1.0.md",
)
SKIP = {".git", ".venv", "venv", "node_modules", "managed_components", "build", "dist", "__pycache__",
        ".local", ".idea", ".vscode", ".pytest_cache"}


def check(publication: bool = False) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    notes: list[str] = []
    for rel in REQUIRED:
        if not (ROOT / rel).is_file():
            errors.append(f"Missing required file: {rel}")

    expected_scopes = None
    try:
        scope = json.loads((ROOT / "legal/license-scope.json").read_text())
        if scope["operative_application_identifier"] != APPLICATION:
            errors.append("License scope differs from the adopted application identifier.")
        root_license = (ROOT / "LICENSE").read_text()
        if any(f"LICENSES/{name}.txt" not in root_license for name in (APPLICATION, PROSE)):
            errors.append("Root LICENSE must identify the application and prose scopes.")
        shield = (ROOT / "legal/reference/LICENSE.PolyForm-Shield-1.0.0.txt").read_bytes()
        supplement = (ROOT / "legal/terms/community-permissions-v1.0.md").read_bytes()
        combined = (ROOT / f"LICENSES/{APPLICATION}.txt").read_bytes()
        if not combined.endswith(shield + b"\n" + supplement):
            errors.append("Application package must reproduce the unchanged base and adopted supplement.")
        reference = (ROOT / "legal/reference/LICENSE.CC-BY-4.0.txt").read_bytes()
        if (ROOT / f"LICENSES/{PROSE}.txt").read_bytes() != reference:
            errors.append("The active CC BY text differs from the recorded reference.")
        expected_scopes = {
            APPLICATION: sorted(scope["application_paths"]),
            PROSE: sorted(scope["documentation_paths"]),
            TZDB: ["apps/controller/main/timezone_data.inc"],
        }
    except (OSError, ValueError, KeyError, TypeError) as exc:
        errors.append(f"Cannot validate license scope: {exc}")

    annotations = []
    try:
        reuse = tomllib.loads((ROOT / "REUSE.toml").read_text())
        annotations = reuse["annotations"]
        if reuse["version"] != 1 or not annotations:
            errors.append("REUSE.toml needs version 1 and actual annotations.")
        scopes = {}
        for entry in annotations:
            license_id = entry["SPDX-License-Identifier"]
            if license_id not in (APPLICATION, PROSE, TZDB):
                errors.append(f"Unreviewed REUSE license mapping: {license_id}")
            scopes.setdefault(license_id, []).extend(entry["path"])
        if expected_scopes and any(sorted(scopes.get(key, [])) != paths for key, paths in expected_scopes.items()):
            errors.append("REUSE scopes differ from the license scope or the IANA exception.")
        notes.append("REUSE scopes checked against the license scope.")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        errors.append(f"Cannot parse license map: {exc}")
        annotations = []

    try:
        records = json.loads((ROOT / "legal/reference/sources.json").read_text())["sources"]
        expected = {"PolyForm-Shield-1.0.0", "CC-BY-4.0"}
        if {record["license"] for record in records} != expected or len(records) != len(expected):
            errors.append("Reference manifest must identify Shield and CC BY once each.")
        for record in records:
            rel = Path(record["path"])
            if rel.is_absolute() or ".." in rel.parts:
                errors.append(f"Unsafe reference path: {rel}")
                continue
            data = (ROOT / rel).read_bytes()
            if hashlib.sha256(data).hexdigest() != record["sha256"] or len(data) != record["bytes"]:
                errors.append(f"Standard-license reference changed: {rel}")
        notes.append("Standard-license local hashes verified; not publisher-signature verification.")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        errors.append(f"Cannot validate standard-license references: {exc}")

    # Check only ordinary inline local Markdown links; deliberately not a full parser.
    markdown_files = [p for p in ROOT.rglob("*.md")
                      if not any(part in SKIP for part in p.relative_to(ROOT).parts)]
    for path in markdown_files:
        text = path.read_text(encoding="utf-8")
        without_fences = re.sub(r"```.*?```", "", text, flags=re.S)
        for target in re.findall(r"\]\(([^)]+)\)", without_fences):
            target = target.strip().split(' "', 1)[0].strip("<>")
            if not target or target.startswith(("#", "http:", "https:", "mailto:")):
                continue
            target = unquote(target.split("#", 1)[0])
            if target and not (path.parent / target).exists():
                errors.append(f"Broken local link in {path.relative_to(ROOT)}: {target}")
    notes.append(f"Local inline Markdown targets checked in {len(markdown_files)} documents.")

    # Check common sensitive filenames and symlinks.
    for path in ROOT.rglob("*"):
        rel = path.relative_to(ROOT)
        if any(part in SKIP for part in rel.parts):
            continue
        if path.is_file() and not path.is_symlink():
            matching = [a for a in annotations if any(fnmatchcase(rel.as_posix(), pat) for pat in a["path"])]
            overrides = [a for a in matching if a.get("precedence") == "override"]
            applicable = {a["SPDX-License-Identifier"] for a in overrides or matching}
            if len(applicable) > 1:
                errors.append(f"Conflicting file scopes: {rel}")
            if applicable:
                with path.open("rb") as file:
                    prefix = file.read(1024)
                header = re.search(rb"(?m)^\s*(?:<!--|//|#|/\*|\*)\s*SPDX-License-Identifier:\s*([^\s]+)", prefix)
                if header and header[1].decode("ascii", errors="replace") not in applicable:
                    errors.append(f"SPDX header disagrees with adopted file scope: {rel}")
        if path.is_symlink():
            errors.append(f"Unexpected symlink: {rel}")
        if path.is_file() and (path.suffix.lower() in {".key", ".p12", ".pfx"}
                               or path.name == ".env"):
            errors.append(f"Sensitive-looking file: {rel}")
    if publication:
        errors.append("PUBLICATION BLOCKED: release publication requires separate maintainer approval.")
    return errors, notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--publication", action="store_true", help="Show the deliberate pre-publication block.")
    args = parser.parse_args()
    errors, notes = check(args.publication)
    for note in notes:
        print(f"INFO: {note}")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("PASS: license scopes and repository checks passed.")
    print("These checks do not authorize publication or establish legal or production readiness.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
