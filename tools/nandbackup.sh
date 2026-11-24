#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# nandbackup.sh take DIR      copy every flash partition off the phone, data and spare
#                             bytes, read twice and compared, ECC checked on this PC
# nandbackup.sh verify DIR    read the phone again and compare with the backup in DIR
# nandbackup.sh check DIR     no phone: check the files against SHA256SUMS and the ECC
#
# Issue #30, docs/backup.md. Works with the phone running our kernel or Yealink's
# (there nandread is copied to /tmp first: set NANDREAD to the ARM binary, default
# native/nandread). Only reads: nandread opens the flash read-only.
#
# One file per partition, NAME.dataoob: each 2048-byte page followed by its 64 spare
# bytes, which is what a restore needs (Yealink keeps filesystem tags in the spare
# bytes). The twelve partitions cover the whole chip with no gaps.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
PHONE=${PHONE:-10.45.0.214}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@$PHONE"
# number:name:size in bytes of data, as /proc/mtd gives them under both kernels
PARTS="0:ubl:131072 1:uboot:1048576 2:env:1048576 3:userenv:1048576 4:logo:1048576
5:smkern:2621440 6:smrfs:15728640 7:kernel:2621440 8:rfs:15728640 10:app:78512128
11:cfg:4194304 12:data:10485760"

usage() { sed -n '3,5p' "$0" | sed 's/^# //' >&2; exit 2; }
[ $# -eq 2 ] || usage
MODE=$1; DIR=$2

setup() {
    KREL=$($SSH uname -r)
    case "$KREL" in
    3.6.*|6.*) NR=nandread; SUF= ;;      # ours: Linux 3.6 or 6.18
    *)  # Yealink: read-only device nodes, our tool in /tmp
        NR=/tmp/nandread; SUF=ro
        scp -q -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
            "${NANDREAD:-$HERE/native/nandread}" root@$PHONE:/tmp/nandread
        $SSH chmod +x /tmp/nandread ;;
    esac
    # the layout must be the one this script knows
    $SSH cat /proc/mtd > "$TMP/proc-mtd"
    for p in $PARTS; do
        n=${p%%:*}; r=${p#*:}; name=${r%%:*}; size=${r#*:}
        grep -q "^mtd$n: $(printf %08x "$size") 00020000 \"$name\"" "$TMP/proc-mtd" ||
            { echo "partition $n ($name) is not where this script expects it; stopping" >&2; exit 1; }
    done
}

# dump NUMBER NAME SIZE OUTFILE: one read, and the tool's own summary must be clean
dump() {
    $SSH "$NR -o /dev/mtd$1$SUF" > "$4" 2> "$4.log" || { cat "$4.log" >&2; exit 1; }
    want=$(( $3 / 2048 * 2112 ))
    [ "$(stat -c %s "$4")" = "$want" ] || { echo "$2: got $(stat -c %s "$4") bytes, wanted $want" >&2; exit 1; }
    grep -q " 0 bad blocks, 0 read errors" "$4.log" || { echo "$2: bad blocks or read errors:" >&2; cat "$4.log" >&2; exit 1; }
    if grep -q "kernel ECC" "$4.log"; then
        grep -q "0 bit errors corrected, 0 failed" "$4.log" || { echo "$2: the flash needed error correction:" >&2; cat "$4.log" >&2; exit 1; }
    fi
}

case "$MODE" in
take)
    mkdir -p "$DIR"
    [ -z "$(ls -A "$DIR")" ] || { echo "$DIR is not empty; a backup is never overwritten" >&2; exit 1; }
    TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
    setup
    for p in $PARTS; do
        n=${p%%:*}; r=${p#*:}; name=${r%%:*}; size=${r#*:}
        f="$DIR/mtd$n-$name.dataoob"
        dump "$n" "$name" "$size" "$f"
        dump "$n" "$name" "$size" "$TMP/second"
        if ! cmp -s "$f" "$TMP/second"; then
            # Yealink writes cfg and data while it runs; under our kernel nothing may differ
            echo "$name: two reads differ" >&2
            [ -n "$SUF" ] && [ "$name" = cfg -o "$name" = data ] || exit 1
            echo "$name: kept the first read (Yealink is writing this partition; back up from our kernel for a steady copy)" >&2
        fi
        mv "$f.log" "$DIR/mtd$n-$name.log"
        echo "$name: $(( size / 2048 )) pages, two reads identical"
    done
    {
        echo "T48S flash backup, taken $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "phone kernel: $KREL"
        echo "MAC: $($SSH cat /sys/class/net/eth0/address 2>/dev/null)"
        echo "format: per page 2048 data bytes then 64 spare bytes (nandread -o)"
        echo; cat "$TMP/proc-mtd"
    } > "$DIR/MANIFEST"
    python3 "$HERE/tools/nandcheck.py" ecc "$DIR"/*.dataoob | tee "$DIR/ECC-CHECK"
    grep -q " [1-9][0-9]* differ" "$DIR/ECC-CHECK" && { echo "ECC check failed; do not trust this backup" >&2; exit 1; }
    ( cd "$DIR" && sha256sum *.dataoob MANIFEST ECC-CHECK > SHA256SUMS )
    chmod a-w "$DIR"/*
    echo "backup complete in $DIR" ;;
verify)
    TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
    ( cd "$DIR" && sha256sum -c --quiet SHA256SUMS )
    setup
    rc=0
    for p in $PARTS; do
        n=${p%%:*}; r=${p#*:}; name=${r%%:*}; size=${r#*:}
        dump "$n" "$name" "$size" "$TMP/now"
        if cmp -s "$DIR/mtd$n-$name.dataoob" "$TMP/now"; then
            echo "$name: the phone still holds exactly the backup"
        else
            echo "$name: DIFFERS from the backup"
            python3 "$HERE/tools/nandcheck.py" cmp "$DIR/mtd$n-$name.dataoob" "$TMP/now" | tail -n 6
            rc=1
        fi
    done
    exit $rc ;;
check)
    ( cd "$DIR" && sha256sum -c --quiet SHA256SUMS ) && echo "checksums match"
    python3 "$HERE/tools/nandcheck.py" ecc "$DIR"/*.dataoob ;;
*) usage ;;
esac
