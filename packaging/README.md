<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Distribution Packaging

## Rehearse a controller package

Activate the pinned ESP-IDF environment and run from the repository root.
Choose a new local directory for the rehearsal.

```sh
source ~/.config/esp32-dev/activate.bash
python3 packaging/prepare_controller.py build/controller-rehearsal --revision HEAD
(cd build/controller-rehearsal && sha256sum -c SHA256SUMS)
```

The tool builds from a Git archive of the selected commit in a fresh
directory. It pins the SDK and compiler, checks the managed dependency
lock, and embeds the short commit ID in the application image. It packages
the initial installation and app-only firmware inputs, the effective build
configuration, the original source archive, the applicable license texts, and
the listed upstream notices. The manifest records the full commit, compiler,
dependency fingerprint, flash offsets and linked archive inventory for the
application and bootloader. The expected archives and notice locations are
in [controller-inputs.toml](controller-inputs.toml).

The linker check detects changes to archives with allocated sections and
checks that the optional Mozilla trust bundle remains outside the image.
The inventory is evidence for a release review. It does not establish the
terms of every input file or prove that outputs from different hosts are
byte-identical. Review actual linked components and notices before any
public distribution.

The bundle includes the license map and terms. Controller USB installation
and recovery are documented in the
[development guide](../docs/development.md#build-and-flash).
