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

## StartOS packages

The `dispatch-preview` package targets StartOS 0.4.2 and newer. The packager
builds and signs a native `.s9pk` for each of x86_64 and aarch64 from one Git
commit. It downloads the pinned Start CLI when needed and verifies both
package signatures and every Merkle archive entry. `STARTOS-SOURCE.json`
records the commit and signing public-key fingerprint. The signed archive
includes release provenance, application license texts, and upstream notices.

### Local rehearsal

Use Linux, Docker Buildx with a multi-architecture container builder, Node.js
22, npm, Python 3.11 or later, OpenSSL and curl. Choose an output directory
that does not yet exist. Generate the development signing key once and reuse
it for upgrades on a rehearsal node.

```sh
umask 077
mkdir -p "$HOME/.config/dispatch-startos"
openssl genpkey -algorithm Ed25519 -out "$HOME/.config/dispatch-startos/development.pem"
docker buildx create --name dispatch-startos --driver docker-container \
  --driver-opt image=moby/buildkit@sha256:baf6138eb8c37644800c7c913fba051e840ebb9ba13eaf92d92ef9a1712d0b4c
BUILDX_BUILDER=dispatch-startos python3 packaging/prepare_startos.py build/startos-rehearsal \
  --revision HEAD --release-version v0.0.1 \
  --signing-key "$HOME/.config/dispatch-startos/development.pem"
(cd build/startos-rehearsal && sha256sum -c DISPATCH-STARTOS-SHA256SUMS)
```

Use the same command with another output directory and a higher version
to exercise a sideloaded upgrade. Verify the service dashboard and health,
observe LAN equipment, and compare settings and history before and after a
restart, upgrade, and StartOS backup restore. See [the operator
guide](../docs/startos.md). When rotating pinned inputs, review the actual
linked libraries and update the associated notices together.

### Approved GitHub releases

Create a protected GitHub Actions environment named `startos-release`
with required maintainer reviewers, no self-approval, and deployments limited
to `main`. Provision a dedicated Ed25519 private key as the
`STARTOS_SIGNING_KEY_PEM` environment secret and put the SHA-256 of its DER
public key in the `STARTOS_SIGNER_SHA256` environment variable. Generate and
retain the private key in the maintainer's private storage. The Atom's OTA
signing key is a separate P-256 key.

```sh
openssl pkey -in /private/release/startos-signer.pem -pubout -outform DER | sha256sum
```

1. After release approval, prepare the controller bundle from the intended
   commit and tag. Push the tag and create a GitHub **draft** for that tag.
2. Archive the existing controller bundle under `dispatch01-usb-vMAJOR.MINOR.PATCH.tar.gz`
   with `manifest.json` at the archive root. Attach it, `dispatch01.bin`,
   and `dispatch01-ota-v1.txt` to the draft.

   ```sh
   tar -C build/controller-v0.1.0 -czf build/dispatch01-usb-v0.1.0.tar.gz .
   ```
3. Run **Attach StartOS to release draft** on `main` with the release tag
   and approve the protected environment. The job verifies the USB
   bundle's source commit and the signed OTA files. It builds and verifies
   both StartOS packages from the same commit and attaches those packages,
   provenance, and checksums to the existing draft. A missing key, signer
   mismatch, verification failure, or upload failure fails the job.
4. Review the green job and the complete draft before the separate publish
   decision. Retry a failed job while the release is still a draft. Retries
   replace only the StartOS assets for that tag and upload their checksum file
   last. StartOS marketplace publication uses its own approval process.
