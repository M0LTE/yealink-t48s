#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Build the T48S root filesystem (and optionally the SDK) with Buildroot.
#   buildroot/build.sh WORKDIR [MAKE TARGETS...]
# Downloads the pinned Buildroot release into WORKDIR, checks its SHA-256, and
# builds t48s_defconfig in WORKDIR/output. With no targets it runs "all", which
# leaves the root filesystem in WORKDIR/output/images/rootfs.cpio; add "sdk" for
# the SDK tarball. Set BR2_DL_DIR and BR2_CCACHE_DIR to share downloads and the
# compiler cache between builds. Host needs: Buildroot's usual packages, plus
# python3-pil and fonts-dejavu-core for our programs' fonts.
set -e
BR_VERSION=2026.08.2
BR_SHA256=bc9497c8e3a734c277e541e2f7fcf9a6275dc92f75bd2f753b05981c54be8705
[ -n "$1" ] || { echo "usage: $0 WORKDIR [MAKE TARGETS...]" >&2; exit 2; }
EXT=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"; WORK=$(cd "$1" && pwd); shift
BR="$WORK/buildroot-$BR_VERSION"
if [ ! -d "$BR" ]; then
    TARBALL="$WORK/buildroot-$BR_VERSION.tar.xz"
    [ -f "$TARBALL" ] || curl -fsSL -o "$TARBALL" "https://buildroot.org/downloads/buildroot-$BR_VERSION.tar.xz"
    echo "$BR_SHA256  $TARBALL" | sha256sum -c -
    tar -C "$WORK" -xf "$TARBALL"
fi
OUT="$WORK/output"
make -C "$BR" O="$OUT" BR2_EXTERNAL="$EXT" t48s_defconfig >/dev/null
[ $# -gt 0 ] || set -- all
make -C "$OUT" "$@"
ls -l "$OUT/images"
