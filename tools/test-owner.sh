#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-owner.sh: rehearse the on-phone installer (t48-install, issue 33,
# docs/installer.md) without the phone.
#
#   test-owner.sh bytes SET BACKUP
#       Any PC. A file stands in for the whole flash, filled from BACKUP, and a
#       folder for the stick. Runs the phone's own t48-install: the backup to
#       the stick and its checks, every refusal, the install's kernel step and
#       the way back, and compares the exact bytes. The app step's UBI work is
#       stood in for by its erase (that part is "ubi" below and
#       tools/test-install.sh ubi).
#
#   test-owner.sh ubi SET
#       Linux, as root, with nandsim, ubi and ubifs. The installer's app step
#       as t48-install runs it on a phone that never had our /data: the whole
#       app partition full of somebody else's data, all 599 blocks erased, UBI
#       made from nothing, slot a written, an empty /data.
#
# SET is an install set (tools/t48install.sh set); BACKUP a flash backup.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
MODE=$1; SET=$2
[ -f "$SET/system.img" ] && [ -f "$SET/T48S-kernel.uimg" ] || { sed -n '5,21p' "$0" | sed 's/^# //' >&2; exit 2; }
SET=$(cd "$SET" && pwd)
INST=$HERE/buildroot/board/t48s/rootfs-overlay/usr/bin/t48-install
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }
sum() { sha256sum < "$1" | cut -d' ' -f1; }

case "$MODE" in
bytes)
    BACKUP=$3; [ -f "$BACKUP/mtd10-app.dataoob" ] || exit 2
    BACKUP=$(cd "$BACKUP" && pwd)
    T=$(mktemp -d); trap 'chmod -R u+w "$T"; rm -rf "$T"' EXIT
    mkdir -p "$T/bin" "$T/proc" "$T/param" "$T/bm" "$T/dev" "$T/lib" "$T/work" "$T/stick/install"
    cc -O2 -Wall -o "$T/bin/nr" "$HERE/native/nandrestore.c"
    order="mtd0-ubl mtd1-uboot mtd2-env mtd3-userenv mtd4-logo mtd5-smkern mtd6-smrfs mtd7-kernel mtd8-rfs mtd10-app mtd11-cfg mtd12-data"
    for p in $order; do cat "$BACKUP/$p.dataoob"; done > "$T/chip"
    S0=$(sum "$T/chip")
    # the phone's programs, pointed at the chip file. Partition numbers as on the phone
    cat > "$T/bin/names" <<'EOS'
name() { case "${1##*/mtd}" in 0) echo ubl 0 131072;; 1) echo uboot 131072 1048576;; 2) echo env 1179648 1048576;;
  3) echo userenv 2228224 1048576;; 4) echo logo 3276800 1048576;; 5) echo smkern 4325376 2621440;;
  6) echo smrfs 6946816 15728640;; 7|14) echo kernel 22675456 2621440;; 8) echo rfs 25296896 15728640;;
  10|15) echo app 41025536 78512128;; 11) echo cfg 119537664 4194304;; 12) echo data 123731968 10485760;; esac; }
EOS
    cat > "$T/bin/nandread" <<EOS
#!/bin/sh
. "$T/bin/names"
for a in "\$@"; do last=\$a; done
set -- \$1 \$(name "\$last")
if [ "\$1" = -b ]; then echo "0 bad blocks" >&2; exit 0; fi
dd if="$T/chip" bs=2112 skip=\$((\$3 / 2048)) count=\$((\$4 / 2048)) 2>/dev/null
echo "\$((\$4 / 2048)) pages, 0 bad blocks, 0 read errors" >&2
EOS
    cat > "$T/bin/nandrestore" <<EOS
#!/bin/sh
. "$T/bin/names"
[ "\$1" = -n ] && exec "$T/bin/nr" "\$@"
args=; for a in "\$@"; do case "\$a" in */mtd*) set -- \$(name "\$a"); part=\$1 ;; *) args="\$args \$a" ;; esac; done
exec "$T/bin/nr" -f "$T/chip" -p "\$part" \$args
EOS
    # the app step, as far as a file can follow it: the erase
    cat > "$T/lib/t48-flash.sh" <<EOS
#!/bin/sh
case "\$1" in
app) echo "\$T48_LOWER" > "$T/lower"; nandrestore -e "\$T48_LOWER" /dev/mtd15 ;;
quiet) : ;;
esac
EOS
    cp "$HERE/buildroot/board/t48s/rootfs-overlay/usr/lib/t48/slots.sh" "$T/lib/"
    chmod +x "$T/bin/"*
    cat > "$T/proc/mtd" <<'EOS'
dev:    size   erasesize  name
mtd14: 00280000 00020000 "t48kernel"
mtd15: 04ae0000 00020000 "t48app"
EOS
    echo "console=tty0 t48boot=live loglevel=8" > "$T/proc/cmdline"
    echo 549 > "$T/param/selftest"; echo 0 > "$T/param/write_errors"; : > "$T/param/lock"
    echo usbmode > "$T/bm/mode"; : > "$T/dev/mtd15"
    : > "$T/stick/T48S.rom"
    cp "$SET/T48S-kernel.uimg" "$SET/system.img" "$T/stick/install/"
    ( cd "$T/stick/install" && sha256sum T48S-kernel.uimg system.img > SHA256SUMS )
    run() {
        PATH=$T/bin:$PATH T48_PROC=$T/proc T48_PARAM=$T/param T48_BOOTMODE=$T/bm T48_LIB=$T/lib T48_WORK=$T/work \
            T48_DEV=$T/dev T48_STICK=$T/stick T48_DROP_CACHES=$T/drop sh "$INST" --machine "$@" > "$T/out" 2>&1
    }
    last() { tail -n 1 "$T/out"; }
    refused() { if run "$1"; then cat "$T/out"; fail "$3"; fi; last | grep -q "^error $2" || { cat "$T/out"; fail "$3"; }; [ "$(sum "$T/chip")" = "$4" ] && pass "$3" || fail "$3: the flash changed"; }

    run status; grep -qx "backup=none" "$T/out" && grep -qx "files=yes" "$T/out" && grep -qx "live=yes" "$T/out" && pass "status: live, stick, install files, no backup" || fail "status"
    refused install "No checked backup" "install without a backup is refused, nothing written" "$S0"
    refused restore "No checked backup" "restore without a backup is refused, nothing written" "$S0"
    run backup || { cat "$T/out"; fail "backup"; }
    last | grep -qx "done Backup checked" || fail "backup did not finish"
    for p in $order; do cmp -s "$T/stick/t48s-backup/$p.dataoob" "$BACKUP/$p.dataoob" || fail "backup of $p differs"; done
    pass "backup: twelve partitions on the stick, each byte for byte the flash, spare bytes included"
    ( cd "$T/stick/t48s-backup" && sha256sum -c --quiet SHA256SUMS ) && [ "$(sum "$T/chip")" = "$S0" ] && pass "backup: checksums right, flash untouched" || fail "backup checksums"
    run backup && last | grep -qx "done Backup already on the stick" && pass "a second backup keeps the first" || fail "second backup"
    # a backup that has rotted on the stick
    f=$T/stick/t48s-backup/mtd10-app.dataoob
    printf 'X' | dd of="$f" bs=1 seek=5000000 conv=notrunc 2>/dev/null
    refused install "Backup on the stick is damaged" "install with a damaged backup is refused" "$S0"
    cp "$BACKUP/mtd10-app.dataoob" "$f"
    # a backup of some other phone
    cp "$T/chip" "$T/chip.keep"
    printf '\125' | dd of="$T/chip" bs=1 seek=$((3276800 / 2048 * 2112 + 9)) conv=notrunc 2>/dev/null
    S1=$(sum "$T/chip")
    refused install "Backup is not this phone" "install with another phone's backup is refused" "$S1"
    cp "$T/chip.keep" "$T/chip"
    # one that was cut short is kept aside and a new one made
    rm "$T/stick/t48s-backup/VERIFIED"
    run status; grep -qx "backup=unfinished" "$T/out" || fail "an unfinished backup is not noticed"
    refused install "No checked backup" "install with an unfinished backup is refused" "$S0"
    run backup && [ -d "$T/stick/t48s-backup.part1" ] && pass "an unfinished backup is kept aside and a new one made" || fail "unfinished backup"
    # damaged install files
    cp "$T/stick/install/system.img" "$T/sys.keep"; printf 'X' >> "$T/stick/install/system.img"
    refused install "Install files on the stick are damaged" "install with damaged install files is refused" "$S0"
    cp "$T/sys.keep" "$T/stick/install/system.img"
    # not a live start
    echo "console=tty0 t48boot=flash" > "$T/proc/cmdline"
    refused install "Not started from a stick" "install on a phone not started from a stick is refused" "$S0"
    echo "console=tty0 t48boot=live loglevel=8" > "$T/proc/cmdline"

    # the install
    run install || { cat "$T/out"; fail "install"; }
    last | grep -qx "done Installed. Take the stick out before restarting" && pass "install: finished; says so when BOOTMODE could not be cleared" || { cat "$T/out"; fail "install end"; }
    [ "$(cat "$T/lower")" = 599 ] && pass "install: a phone that never had our /data has all 599 blocks of app erased" || fail "erase count"
    part() { dd if="$T/chip" bs=2112 skip=$(($1 / 2048)) count=$(($2 / 2048)) 2>/dev/null; }
    python3 "$HERE/tools/t48flash.py" kernelimage "$SET/T48S-kernel.uimg" "$T/k.want" >/dev/null
    part 22675456 2621440 > "$T/k"
    cmp "$T/k" "$T/k.want" && pass "install: kernel partition is exactly the image a PC works out, ECC bytes included" || fail "kernel bytes"
    python3 "$HERE/tools/t48flash.py" nboot "$T/k" "$SET/T48S-kernel.uimg" | tail -n 1 | sed 's/^/     /'
    for p in "0 22675456" "25296896 15728640" "119537664 14680064"; do
        set -- $p; [ "$(part $1 $2 | sha256sum)" = "$(dd if="$T/chip.keep" bs=2112 skip=$(($1 / 2048)) count=$(($2 / 2048)) 2>/dev/null | sha256sum)" ] || fail "something outside kernel and app changed"
    done
    pass "install: nothing outside the kernel and app partitions changed"
    grep -q "close-kernel" "$T/param/lock" || true
    echo none > "$T/bm/mode"
    # the way back
    run restore || { cat "$T/out"; fail "restore"; }
    last | grep -qx "done Restored" || fail "restore end"
    [ "$(sum "$T/chip")" = "$S0" ] && pass "restore: the whole flash is the backup again, byte for byte: $S0" || fail "restore bytes"
    echo "ALL PASSED (bytes)" ;;

ubi)
    [ "$(id -u)" = 0 ] || { echo "needs root" >&2; exit 1; }
    T=$(mktemp -d); U=7
    export T48_UBI=$U T48_UBISYS=/sys/class/ubi
    cleanup() {
        for m in $(sed -n "s/^ubi$U[:_][^ ]* \([^ ]*\) .*/\1/p" /proc/mounts); do umount "$m" 2>/dev/null || true; done
        ubidetach -d $U /dev/ubi_ctrl 2>/dev/null || true
        rmmod nandsim 2>/dev/null || true
        rm -rf "$T"
    }
    trap cleanup EXIT
    modprobe ubi; modprobe ubifs
    mkdir -p "$T/inst" "$T/mnt"
    printf '#!/bin/sh\nexec %s -b 2 "$@"\n' "$(command -v ubiattach)" > "$T/inst/ubiattach"; chmod +x "$T/inst/ubiattach"
    PATH=$T/inst:$PATH
    modprobe nandsim first_id_byte=0xef second_id_byte=0xf1 third_id_byte=0x00 fourth_id_byte=0x95 parts=313,599
    APP=$(sed -n 's/^mtd\([0-9]*\): 04ae0000 00020000 "NAND simulator.*/\1/p' /proc/mtd | head -n 1)
    [ -n "$APP" ] || fail "no app partition on the simulated chip"
    head -c $((599 * 131072)) /dev/urandom > "$T/yealink"
    nandwrite -q /dev/mtd$APP "$T/yealink"
    pass "simulated chip: app is mtd$APP, all 599 blocks full of somebody else's data"
    export T48_MTD_t48app=$APP T48_MTD_t48kernel=none T48_LOCK=$T/lock T48_PARAM=$T T48_LOWER=599
    cc -O2 -o "$T/inst/nandrestore" "$HERE/native/nandrestore.c"
    cp "$HERE/tools/phone/t48-flash.sh" "$HERE/buildroot/board/t48s/rootfs-overlay/usr/lib/t48/slots.sh" "$SET/system.img" "$T/inst/"
    sh "$T/inst/t48-flash.sh" app > "$T/first.log" 2>&1 || { cat "$T/first.log"; fail "the app step stopped"; }
    sed 's/^/     /' "$T/first.log"
    T48_SLOTS=$T/inst/slots.sh; . "$T48_SLOTS"
    [ "$(slot_sum a)" = "$(sum "$SET/system.img")" ] && pass "slot a holds the system image" || fail "slot a"
    state_read; [ "$S_VALID $S_ACTIVE $S_TRIAL" = "1 a -" ] && pass "boot note: slot a active" || fail "boot note"
    mount -t ubifs ubi$U:data "$T/mnt" && [ -z "$(ls -A "$T/mnt")" ] && pass "/data: a new, empty volume that mounts" || fail "/data"
    umount "$T/mnt"
    sh "$T/inst/t48-flash.sh" app > "$T/second.log" && grep -q "already in its new volume" "$T/second.log" && pass "a second run changes nothing" || fail "second run"
    # one start, as the first stage does it
    ubidetach -d $U /dev/ubi_ctrl; ubiattach -m $APP -d $U /dev/ubi_ctrl >/dev/null
    slot_plan
    set -- $SLOT_ORDER
    slot_load "$1" "$T/system.img" && [ "$1" = a ] && cmp -s "$T/system.img" "$SET/system.img" && pass "first start: slot a loads, the image byte for byte" || fail "first start"
    echo "ALL PASSED (ubi)" ;;
*) sed -n '5,21p' "$0" | sed 's/^# //' >&2; exit 2 ;;
esac
