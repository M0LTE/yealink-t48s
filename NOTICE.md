# Notices

## Not Yealink's

This project is not affiliated with, endorsed by or supported by Yealink. "Yealink" and the model names are trademarks of their owner, used here only to say which phones the firmware runs on. No Yealink firmware, code or key is in this repository or in its releases. A backup of a phone's own firmware, made by the installer, stays on that owner's USB stick.

Use it at your own risk. Installing replaces the phone's firmware. The way back is the backup the installer makes first: start from the stick that holds it and choose Settings, Install, Restore Yealink firmware ([docs/guide.md](docs/guide.md)). There is no warranty: see sections 15 and 16 of [LICENSE](LICENSE).

## Our own code

Copyright the yealink-t48s authors.

- AGPL-3.0-or-later ([LICENSE](LICENSE)): everything not named below.
- GPL-2.0: code built into or loaded into a Linux kernel (`kernel/drivers/`, `kernel6/drivers/`, `kernel6/dts/`, `kernel6/patches/`, `kmod/`), because Linux is GPL-2.0 only.

Each file says which in its `SPDX-License-Identifier` line. A patch carries the licence of the code it changes. Pictures, data files and generated tables without a line are under the licence of the folder's code.

## Third-party code in this repository

| What | Where | Licence |
|---|---|---|
| speexdsp's echo canceller (Xiph.Org, Jean-Marc Valin and others), cut down, one function added | `audiod/third_party/speexdsp/` | BSD-3-Clause, `audiod/third_party/speexdsp/COPYING` |
| B612 and B612 Mono (the Eclipse Foundation's B612 project), IBM Plex Sans and IBM Plex Mono (IBM), Figtree (Erik Kennedy): the font files, and glyph tables drawn from them in `libt48/src/fonts_*.c` | `libt48/fonts/`, `webconfig/fonts/` (the same faces as WOFF2, for the web setup page) | SIL Open Font Licence 1.1: `libt48/fonts/OFL-B612.txt`, `OFL-IBMPlex.txt`, `OFL-Figtree.txt` |
| DejaVu Sans and DejaVu Sans Mono, as glyph tables for the boot banner | `native/gen/fonts.h` | Bitstream Vera licence, DejaVu's changes public domain: `native/gen/LICENSE-DejaVu.txt` |
| A patch to re for Linux 3.6 headers | `sip/patches/` | BSD-3-Clause, as re |
| Patches to Linux 6.18 | `kernel6/patches/` | GPL-2.0, as Linux |

## Third-party programs in what a release ships

They are downloaded and built by CI, not kept here. Versions are pinned in `buildroot/build.sh`, `buildroot/package/`, `kernel/` and `kernel6/build.sh`; the exact list for a release, with every licence text, is the manifest in its source archive (below).

| What | In | Licence |
|---|---|---|
| Linux 6.18 (the phone's kernel) and Linux 3.6.11 (the fallback kernel; its headers) | the kernel files, the stick | GPL-2.0 |
| busybox | the system image and the first stage | GPL-2.0 |
| musl | linked into every program | MIT |
| GCC's runtime library (libgcc) | linked into every program | GPL-3.0-or-later with the GCC Runtime Library Exception |
| dropbear | the system image | MIT, with parts under BSD-2-Clause and public domain |
| curl | the system image | curl |
| Mbed TLS | the system image (curl) | Apache-2.0 or GPL-2.0-or-later |
| OpenSSL | the system image (baresip: SIP over TLS, SRTP) | Apache-2.0 |
| ca-certificates (Mozilla's list) | the system image | MPL-2.0 |
| baresip and re (libre) | the system image | BSD-3-Clause |
| libg722 | the system image | BSD-2-Clause and public domain |
| libopus (Xiph.Org, Skype, Jean-Marc Valin and others) | the system image | BSD-3-Clause |
| GCC, binutils and the other host tools | the app SDK (`t48s-sdk.tar.gz`) | GPL-3.0-or-later, GPL-2.0-or-later and others: the host manifest lists each |
| Buildroot, which builds all of the above | not shipped | GPL-2.0-or-later |

The licence texts of baresip, re, libg722, libopus and OpenSSL are also on the phone, in `/usr/share/t48/sip/licences`.

## Source for the binaries

Every `v*` release has an asset `t48s-sources.tar.gz`: the source archive and patches of each package above as Buildroot's `make legal-info` collects them, with licence texts and manifests; the Buildroot release; the Linux 6.18 and 3.6.11 archives from kernel.org; and this repository at the release's commit, which holds our kernel drivers, patches, device trees and configurations. Its `README.txt` says what is where.

The rolling builds (`system-latest`, `sdk-latest`, `t46s-latest`) are made from this repository at the commit their notes name, with the same pinned third-party versions; a run of the build workflow started by hand produces the same archive for any commit. Anyone who has a binary from this project and cannot get its source that way can ask for it in an issue at <https://github.com/M0LTE/yealink-t48s/issues>, for at least three years after that binary was published.
