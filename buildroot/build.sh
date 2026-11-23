#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Build the T48S root filesystem (and optionally the SDK) with Buildroot.
#   buildroot/build.sh WORKDIR [MAKE TARGETS...]
# Downloads the pinned Buildroot release into WORKDIR, checks its SHA-256, and
# builds t48s_defconfig in WORKDIR/output. With no targets it runs "all", which
# leaves the root filesystem in WORKDIR/output/images/rootfs.cpio; add "sdk" for
# the SDK tarball. Set BR2_DL_DIR and BR2_CCACHE_DIR to share downloads and the
# compiler cache between builds. Host needs: Buildroot's usual packages.
# The pin follows Buildroot's long-term (YYYY.02.x) line.
set -e
BR_VERSION=2026.02.3
BR_SHA256=5a59e7501b0b4ec52c41f4bfa79412320e0b37eae5f719605a258e8d0c6fc7fb
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
# T48S_BR_FRAGMENT: a file of options laid over the defconfig (configs/kernel6.fragment)
if [ -n "$T48S_BR_FRAGMENT" ]; then
    FRAG=$(cd "$(dirname "$T48S_BR_FRAGMENT")" && pwd)/$(basename "$T48S_BR_FRAGMENT")
    (cd "$OUT" && "$BR/support/kconfig/merge_config.sh" -m .config "$FRAG" >/dev/null)
    make -C "$OUT" olddefconfig >/dev/null
    grep -v '^#' "$FRAG" | grep . | while read -r o; do
        grep -qxF "$o" "$OUT/.config" || { echo "fragment option not taken: $o" >&2; exit 1; }
    done
fi
[ $# -gt 0 ] || set -- all
make -C "$OUT" "$@"
ls -l "$OUT/images"
