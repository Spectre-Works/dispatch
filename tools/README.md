<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Development tools

| Tool | When to use it |
| --- | --- |
| [check.sh](check.sh) | Run the repository checker and host C tests. Add `--with-idf` from an activated ESP-IDF shell to run the mining-path fixtures. |
| [check_repository.py](check_repository.py) | Check local Markdown links, reference-license hashes, and license scopes and repository conventions using Python 3.11+ and its standard library. |
| [dispatch_pair.py](dispatch_pair.py) | Open a private USB setup session or print a dashboard sign-in QR. |
| [dispatch_miner_presets.py](dispatch_miner_presets.py) | Collect read-only Goldshell compatibility evidence from a specified miner. |
| [generate_controller_timezones.py](generate_controller_timezones.py) | Rebuild the embedded time-zone table from the pinned tzdata. [Regeneration instructions](../docs/development.md#refresh-embedded-time-zone-data) |

## USB pairing

`dispatch_pair.py` requires Python 3 and the `pyserial` package. Use the
activated ESP-IDF environment if it already provides pySerial, or install it
in a local Python virtual environment with `python -m pip install pyserial`.
Install `qrencode` for terminal QR codes. Run pairing from a private
interactive terminal because it displays keys and QR codes.
[Provisioning steps](../docs/development.md#provision-the-controller)
