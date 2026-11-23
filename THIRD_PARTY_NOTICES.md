<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->

# Third-Party Material Inventory

No third-party application source or build output is bundled in this
repository. Standard-license reference documents are present under
`legal/reference/`; their provenance and local content hashes are recorded
there. Their inclusion does not place DISPATCH under those licenses.

Add actual dependencies here before distribution. Record component, version,
source, license, incorporated locations, changes, and required redistribution
notices or source obligations. Record assets and copied snippets as well as
package-manager dependencies. Do not record credentials or private contracts.

This inventory is not an Apache `NOTICE` file and does not alter upstream
license grants. When an actual Apache-licensed dependency supplies notices,
preserve them through the relevant release artifacts.

## Controller development build

The build links components from locally installed Espressif ESP-IDF v6.1.1.
No upstream sources or generated firmware binaries are checked in here.
The development build uses the upstream files without patches. Relevant
upstream license and notice locations in the pinned IDF checkout include:

| Input | Upstream license location |
| --- | --- |
| ESP-IDF platform, Wi-Fi, PHY, and coexistence binary libraries | SDK root `LICENSE` and `lib/LICENSE` in the respective SDK components (Apache 2.0) |
| FreeRTOS kernel | `components/freertos/FreeRTOS-Kernel/LICENSE.md` (MIT) |
| lwIP | `components/lwip/lwip/COPYING` (BSD terms) |
| Xtensa port assembly and interrupt code | Cadence MIT notice in `components/xtensa/xtensa_context.S` and matching linked port sources |
| Undefined-behavior handlers | BSD-2-Clause notice in `components/esp_system/ubsan.c` |
| TLSF heap allocator | `components/heap/tlsf` at revision `2867f6883a12920b1969ff9624c0ab0e4185c2ce` (BSD-3-Clause). [Original author notice](LICENSES/LicenseRef-TLSF-BSD-3-Clause.txt) taken from [tlsf.h at deff9ab](https://github.com/mattconte/tlsf/blob/deff9ab509341f264addbd3c8ada533678591905/tlsf.h). |
| WPA supplicant | `components/wpa_supplicant/COPYING` (BSD-3-Clause) |
| HTTP parser | `components/http_parser/LICENSE.txt` (upstream NGINX and Node/Joyent notices) |
| ESP-TLS for MQTT | `components/esp-tls` file headers and the SDK's root `LICENSE` (Apache 2.0) |
| Mbed TLS and TF-PSA-Crypto | `components/mbedtls/mbedtls/LICENSE` and `tf-psa-crypto/LICENSE` under the same directory (Apache 2.0 option) |
| GCC runtime and C++ library | Pinned toolchain `share/licenses/gcc/COPYING3` and `COPYING.RUNTIME` (GPL-3.0 with the GCC Runtime Library Exception where applicable) |
| Picolibc and inherited Newlib code | Pinned toolchain `share/licenses/picolibc/COPYING.picolibc` and `COPYING.NEWLIB` (upstream component terms) |

Shelly discovery links the unmodified [Espressif mDNS component 1.9.0](https://components.espressif.com/components/espressif/mdns/versions/1.9.0),
under Apache 2.0. The build fetches it into `apps/controller/managed_components/`
using the component manifest and lock file. Preserve its upstream `LICENSE`
and any applicable notices with a distribution.

The MQTT adapter is first-party protocol code. It uses the unmodified
ESP-TLS and Mbed TLS components in the pinned SDK. A broker CA certificate
is owner-provided configuration. The linked controller application does not
include the SDK's optional Mozilla CA bundle. [Integration provenance](docs/compatibility.md#protocol-provenance)

`apps/controller/main/timezone_data.inc` contains derived timezone names and
transitions from the public-domain [IANA Time Zone Database 2026f](https://data.iana.org/time-zones/releases/tzdata2026f.tar.gz).
See the [upstream license declaration](LICENSES/LicenseRef-IANA-TZDB-Public-Domain.txt).

The USB setup tool uses the installed pySerial package and optionally
invokes the host's qrencode command. They remain host tools and are absent
from the controller image. Preserve upstream notices and audit the IDF
component and toolchain inputs against the actual build before assembling
a distributable firmware artifact. See the
[ESP-IDF v6.1.1 source](https://github.com/espressif/esp-idf/tree/v6.1.1).

The [controller packaging rehearsal](packaging/README.md) copies these license
files and the managed mDNS license directly from the pinned inputs. It
records contributing linker archives for the application and bootloader.
Use that inventory for the per-file and per-binary release review.
