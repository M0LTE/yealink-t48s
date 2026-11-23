#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# nandrestore.sh check DIR      no phone: show that restoring the backup in DIR would put
#                               back exactly its bytes, for every partition
# nandrestore.sh t48data DIR    on the phone, under our kernel: give our part of the
#                               flash (the last 60 blocks of "app") back to stock from
#                               the backup in DIR, then compare the whole flash with it
#
# Issue #30, docs/backup.md. The work on the phone is done by nandrestore
# (native/nandrestore.c); the kernel driver lets it write our own ranges only.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
PHONE=${PHONE:-10.45.0.214}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@$PHONE"
[ $# -eq 2 ] || { sed -n '3,7p' "$0" | sed 's/^# //' >&2; exit 2; }
DIR=$2
( cd "$DIR" && sha256sum -c --quiet SHA256SUMS )
case "$1" in
check)
    TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
    cc -O2 -o "$TMP/nandrestore" "$HERE/native/nandrestore.c"
    for f in "$DIR"/*.dataoob; do
        printf '%s: ' "$(basename "$f" .dataoob)"
        "$TMP/nandrestore" -n -o "$TMP/out" < "$f"
        cmp "$TMP/out" "$f"
    done ;;
t48data)
    # the range compiled into kernel/drivers/t48s_nand.c: app's blocks 539 to 598
    M=$($SSH "sed -n 's/^mtd\([0-9]*\): 00780000 00020000 \"t48data\"\$/\1/p' /proc/mtd")
    [ -n "$M" ] || { echo "the phone's kernel has no t48data partition" >&2; exit 1; }
    $SSH t48-data release
    tail -c $((60 * 64 * 2112)) "$DIR/mtd10-app.dataoob" | $SSH "nandrestore /dev/mtd$M"
    "$HERE/tools/nandbackup.sh" verify "$DIR" ;;
*) exit 2 ;;
esac
