#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Make the app SDK from Buildroot's SDK tarball (buildroot/build.sh WORKDIR sdk).
#   sdk/assemble.sh BUILDROOT_SDK.tar.gz OUT.tar.gz
# Adds to the cross compiler, musl, libt48 and mbedTLS that Buildroot gives:
#   bin/t48-sim            the PC simulator (x86-64 Linux, static)
#   bin/t48-pack           makes .t48app packages and feed indexes
#   share/t48/app.mk       build rules for an app's Makefile
#   share/t48/libt48/      libt48's source, for building apps for the simulator
#   share/t48/README.md, VERSION, api
# Run on an x86-64 Linux PC with a C compiler and static glibc (CI does this).
set -e
[ $# -eq 2 ] || { echo "usage: $0 BUILDROOT_SDK.tar.gz OUT.tar.gz" >&2; exit 2; }
REPO=$(cd "$(dirname "$0")/.." && pwd)
IN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
OUT=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

tar -C "$W" -xzf "$IN"
mv "$W"/*_sdk-buildroot "$W/t48s-sdk"
S="$W/t48s-sdk"
[ -f "$S/arm-buildroot-linux-musleabi/sysroot/usr/lib/libt48.a" ] || { echo "no libt48 in the Buildroot SDK" >&2; exit 1; }

# the simulator: static, so it runs on any x86-64 Linux
rm -rf "$REPO/shell/build-sdk" "$REPO/libt48/build-sdk"
make -C "$REPO/shell" O=build-sdk CC="cc -static" build-sdk/t48-shell >/dev/null
install -m 0755 "$REPO/shell/build-sdk/t48-shell" "$S/bin/t48-sim"
strip "$S/bin/t48-sim"
rm -rf "$REPO/shell/build-sdk" "$REPO/libt48/build-sdk"
install -m 0755 "$REPO/sdk/t48-pack" "$S/bin/t48-pack"

T="$S/share/t48"
mkdir -p "$T/libt48"
cp -r "$REPO/libt48/Makefile" "$REPO/libt48/LICENSE" "$REPO/libt48/README.md" \
      "$REPO/libt48/include" "$REPO/libt48/src" "$REPO/libt48/examples" "$T/libt48/"
install -m 0644 "$REPO/sdk/app.mk" "$REPO/sdk/README.md" "$T/"
sed -n 's/^#define T48_API_LEVEL \([0-9]*\).*/\1/p' "$REPO/libt48/include/t48_proto.h" > "$T/api"
echo "$(git -C "$REPO" describe --tags --always --dirty 2>/dev/null || echo unknown) $(date -u +%Y-%m-%d)" > "$T/VERSION"

tar -C "$W" -czf "$OUT" t48s-sdk
ls -l "$OUT"
