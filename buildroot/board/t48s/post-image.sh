#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Buildroot post-image: the two files a boot is made of (docs/boot.md).
#   system.img       the system image: the root filesystem as squashfs, read-only
#   firststage.cpio  the first stage, for the kernel's built-in initramfs
set -e
cp "$BINARIES_DIR/rootfs.squashfs" "$BINARIES_DIR/system.img"
python3 "$(dirname "$0")/mkfirststage.py" "$TARGET_DIR" "$BINARIES_DIR/firststage.cpio"
