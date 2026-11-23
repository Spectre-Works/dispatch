<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->

# Contributing to DISPATCH

## Make and verify a change

1. Read [AGENTS.md](AGENTS.md) and [LICENSING.md](LICENSING.md). Use the [architecture map](docs/architecture.md)
   to find the owning code and keep changes focused.
2. Run `./tools/check.sh` from the root. With the pinned ESP-IDF activated,
   `./tools/check.sh --with-idf` also exercises integration protocol fixtures.
   Follow the [development guide](docs/development.md) for firmware and
   dashboard verification on a controller.
3. Summarize what changed, which targets it affects, and what you verified.
   Distinguish host checks, simulator behavior, and physical evidence.
   Link any updated operator instructions or consequential decisions.

## Provenance and rights

Record new dependencies and material sources, including AI-assisted or
imported code where provenance or rights need review. Contribute material
you have permission to submit.

Before merging an external core contribution, verify company rights or
complete and execute the [owner-approved CLA form](legal/terms/contributor-license-agreement-v1.0.md)
with the contributor. Record the rights instrument, signer authority, version,
date, and covered submission privately. Confirm the record matches the patch
and separately identify any third-party material. Opening or signing off on a
pull request does not execute the CLA. Keep signatures and personal details
outside Git.

The agreement identifies the rights granted for each accepted contribution.
