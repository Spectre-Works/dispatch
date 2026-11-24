<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0 -->

# Distribution Packaging

## Rehearse a controller package

Activate the pinned ESP-IDF environment and run from the repository root.
Choose a new local directory for the rehearsal.

```sh
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

The bundle includes the license map and terms. To provision a blank board
from the bundle, verify the target and follow the
[initial USB installation instructions](../docs/development.md#build-and-flash).
The packaged `firmware/flash_args` contains the initial image offsets and
settings for esptool. Run it from the package's `firmware` directory.

## Prepare a versioned USB and Network Update package

Use the same packager with a release version, a PEM file containing the
release hosts' trusted public roots, and the private ECDSA P-256 signing key.
It builds a single app from the selected commit and uses identical app bytes
for USB and Network Update. Keep the private signer file outside the checkout
and use OpenSSL from the build host.

```sh
python3 packaging/prepare_controller.py build/controller-v0.1.0 \
  --revision HEAD --release-version v0.1.0 \
  --ota-ca /private/release/github-roots.pem \
  --signing-key /private/release/signer.key
(cd build/controller-v0.1.0 && sha256sum -c SHA256SUMS)
```

The result contains the initial USB image set, `ota/dispatch01.bin` and
`ota/dispatch01-ota-v1.txt`, the public trust inputs, and the source and
provenance for that version. The packager checks that the app embeds the
supplied trust inputs and verifies the signed manifest before finishing.
The smallest app slot limits the image to 3.25 MiB. For a manually built
bench image, [the stand-alone signing tool](prepare_network_update.py)
can generate just the OTA manifest.

For an approved GitHub release, use the embedded version as the tag and
attach both files from `ota/`. Attach the USB package for initial setup.
The controller checks the latest release permalink over anonymous HTTPS,
then fetches the image from the signed version's release path. Review the
CA provenance and trust chains for GitHub and its release-asset host.
For a same-LAN test, pass `--ota-release-base https://BENCH_IP:8443/releases`
with the bench CA and signer. The package manifest records the selected
feed and public trust fingerprints.

Use a controller provisioned with the rollback bootloader and current
layout. Owner keys, HTTPS roots, and release signing keys serve separate
purposes. Keep a private copy of the release signer and a known-good USB
package. If the release trust inputs need replacing and the existing
update path is unavailable or untrusted, provision the new trust over USB.
[Bench and USB recovery instructions](../docs/development.md#network-update-bench)
