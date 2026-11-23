#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Buildroot post-build: our own /etc/init.d/rcS starts everything, so drop the
# S?? scripts that packages install for Buildroot's stock rcS (never run here).
set -e
TARGET_DIR=$1
rm -f "$TARGET_DIR"/etc/init.d/S[0-9][0-9]*
# t48-shell's folders (docs/shell.md, docs/phone.md): installed apps, their
# data, settings, call history. In RAM for now, like the rest of the image.
mkdir -p "$TARGET_DIR"/data/apps "$TARGET_DIR"/data/appdata "$TARGET_DIR"/data/config \
    "$TARGET_DIR"/data/phone
# root's SSH keys live with the rest of what the owner has set (docs/access.md)
rm -rf "$TARGET_DIR"/root/.ssh
ln -s /data/access/ssh "$TARGET_DIR"/root/.ssh
# dropbear's own start script and key folder are not used: t48-access runs it
rm -f "$TARGET_DIR"/etc/dropbear
# The system image is read-only (docs/boot.md). Our test builds' keys are mounted
# on /etc/t48-dev by the first stage.
mkdir -p "$TARGET_DIR"/etc/t48-dev
chmod 700 "$TARGET_DIR"/etc/t48-dev
