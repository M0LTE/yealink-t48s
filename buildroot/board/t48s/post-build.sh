#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Buildroot post-build: our own /etc/init.d/rcS starts everything, so drop the
# S?? scripts that packages install for Buildroot's stock rcS (never run here).
set -e
TARGET_DIR=$1
rm -f "$TARGET_DIR"/etc/init.d/S[0-9][0-9]*
# t48-shell's folders (docs/shell.md): installed apps, their data, settings
mkdir -p "$TARGET_DIR"/data/apps "$TARGET_DIR"/data/appdata "$TARGET_DIR"/data/config
