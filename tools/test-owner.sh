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
# SET is an install set (tools/t48install.sh set); BACKUP a flash backup. With
# T46S-kernel.uimg in SET too (kernel6/build.sh MODEL=t46s FLASH=1), "bytes"
# also runs the install and the way back as a T46S does them, on the same flash.
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
    cc -O2 -Wall -o "$T/bin/t48-meter" "$HERE/native/t48-meter.c"
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
    echo matches > "$T/param/layout"
    # which phone, as t48-model reads it: the device tree and the Holtek chip
    ln -s "$HERE/buildroot/board/t48s/rootfs-overlay/usr/bin/t48-model" "$T/bin/t48-model"
    mkdir -p "$T/dt"; printf 'yealink,sip-t48s\0yealink,sip-t4xs\0' > "$T/dt/compatible"; echo 65 > "$T/hwid"
    export T48_DT="$T/dt" T48_HWID="$T/hwid"
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

    run status; grep -qx "backup=none" "$T/out" && grep -qx "files=yes" "$T/out" && grep -qx "live=yes" "$T/out" &&
        grep -qx "model=t48s" "$T/out" && grep -qx "layout=matches" "$T/out" && pass "status: live, a T48S, layout known, stick, install files, no backup" || fail "status"
    refused install "No checked backup" "install without a backup is refused, nothing written" "$S0"
    refused restore "No checked backup" "restore without a backup is refused, nothing written" "$S0"
    run backup || { cat "$T/out"; fail "backup"; }
    last | grep -qx "done Backup checked" || fail "backup did not finish"
    for p in $order; do cmp -s "$T/stick/t48s-backup/$p.dataoob" "$BACKUP/$p.dataoob" || fail "backup of $p differs"; done
    pass "backup: twelve partitions on the stick, each byte for byte the flash, spare bytes included"
    # progress by bytes: it starts at nothing, never goes back, and both halves reach the end
    grep -qx "step 0 Copying, 0 of 138 MB" "$T/out" && grep -q "^step [0-9]* Copying, 138 of 138 MB$" "$T/out" &&
        grep -q "^step [0-9]* Checking, 0 of 138 MB$" "$T/out" && grep -q "^step 99 Checking, 138 of 138 MB$" "$T/out" &&
        sed -n 's/^step \([0-9]*\) .*/\1/p' "$T/out" | sort -n -c &&
        pass "backup: progress in MB copied and checked, $(grep -c '^step' "$T/out") steps, never backwards" || { grep '^step' "$T/out" | tail -n 5; fail "backup progress"; }
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
    # a flash whose layout the driver did not find (docs/models.md)
    echo "mtdparts is not this layout" > "$T/param/layout"
    run status; grep -qx "layout=unknown" "$T/out" || fail "status does not say the layout is unknown"
    refused install "Flash layout not known" "install on a flash whose layout is not known is refused" "$S0"
    refused restore "Flash layout not known" "restore on a flash whose layout is not known is refused" "$S0"
    echo matches > "$T/param/layout"
    # the chip has not answered: the model is not certain enough to write
    echo 0 > "$T/hwid"
    refused install "Phone model not known" "install with the phone's model in doubt is refused" "$S0"
    echo 65 > "$T/hwid"
    # a kernel file named for the other model
    cp "$T/stick/install/T48S-kernel.uimg" "$T/k.keep"
    python3 -I -c "
import sys
d = bytearray(open(sys.argv[1], 'rb').read()); d[32:64] = b'Linux-6.18.65-t46s'.ljust(32, b'\0')
open(sys.argv[1], 'wb').write(d)" "$T/stick/install/T48S-kernel.uimg"
    ( cd "$T/stick/install" && sha256sum T48S-kernel.uimg system.img > SHA256SUMS )
    refused install "Kernel file is not for this phone" "a T46S kernel on a T48S is refused" "$S0"
    cp "$T/k.keep" "$T/stick/install/T48S-kernel.uimg"
    ( cd "$T/stick/install" && sha256sum T48S-kernel.uimg system.img > SHA256SUMS )
    # not a live start
    echo "console=tty0 t48boot=flash" > "$T/proc/cmdline"
    refused install "Not started from a stick" "install on a phone not started from a stick is refused" "$S0"
    echo "console=tty0 t48boot=live loglevel=8" > "$T/proc/cmdline"

    # the install
    run install || { cat "$T/out"; fail "install"; }
    last | grep -qx "done Installed" && pass "install: finished" || { cat "$T/out"; fail "install end"; }
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
    grep -q "^step 7[0-5] System, 80 of 80 MB$" "$T/out" && pass "restore: progress in MB written" || { grep '^step' "$T/out" | tail -n 5; fail "restore progress"; }
    # without the meter the work is the same
    mv "$T/bin/t48-meter" "$T/t48-meter"; mv "$T/stick/t48s-backup" "$T/first-backup"
    run backup || { cat "$T/out"; fail "backup without the meter"; }
    for p in $order; do cmp -s "$T/stick/t48s-backup/$p.dataoob" "$BACKUP/$p.dataoob" || fail "backup of $p differs (no meter)"; done
    pass "backup without t48-meter: the same twelve files"
    mv "$T/t48-meter" "$T/bin/t48-meter"
    # the end: the stick is let go, the restart waits until it is out, and the
    # watchdog's feeder is stopped before the reset asks for the watchdog
    mkdir -p "$T/sys/sda"
    printf '#!/bin/sh\necho "sv $*" >> "%s/ends"\n' "$T" > "$T/bin/sv"
    printf '#!/bin/sh\necho "watchdog $* stick=$([ -e "%s/sys/sda" ] && echo in || echo out)" >> "%s/ends"\n' "$T" "$T" > "$T/bin/t48-watchdog"
    chmod +x "$T/bin/sv" "$T/bin/t48-watchdog"
    ( sleep 3; rmdir "$T/sys/sda" ) &
    T48_SYSBLOCK=$T/sys run eject || true
    wait
    grep -qx "step 100 Restarting" "$T/out" && grep -qx "watchdog reset stick=out" "$T/ends" && pass "eject: the restart waits for the stick to come out" || { cat "$T/out" "$T/ends"; fail "eject"; }
    last | grep -qx "error Not restarted. Unplug the power" && pass "a restart that does not happen says so" || fail "restart failure"

    # ---- the same flash as a T46S (issue 10): its own stick names, its own kernel ----
    if [ -f "$SET/T46S-kernel.uimg" ]; then
        rm -f "$T/bin/sv" "$T/bin/t48-watchdog"; rm -rf "$T/stick"; mkdir -p "$T/stick/install"
        cp "$T/chip.keep" "$T/chip"; [ "$(sum "$T/chip")" = "$S0" ] || fail "chip copy"
        printf 'yealink,sip-t46s\0yealink,sip-t4xs\0' > "$T/dt/compatible"; echo 66 > "$T/hwid"; echo usbmode > "$T/bm/mode"
        : > "$T/stick/T48S.rom"
        run status; grep -qx "model=t46s" "$T/out" && grep -qx "stick=no" "$T/out" && pass "T46S: a T48S stick is not its stick" || fail "T46S status with a T48S stick"
        rm "$T/stick/T48S.rom"; : > "$T/stick/T46S.rom"
        cp "$SET/T48S-kernel.uimg" "$SET/system.img" "$T/stick/install/"
        ( cd "$T/stick/install" && sha256sum T48S-kernel.uimg system.img > SHA256SUMS )
        run status; grep -qx "stick=yes" "$T/out" && grep -qx "files=no" "$T/out" && pass "T46S: its stick, and the T48S's install files are not its install files" || fail "T46S status"
        run backup && last | grep -qx "done Backup checked" && [ -f "$T/stick/t46s-backup/VERIFIED" ] && [ ! -e "$T/stick/t48s-backup" ] &&
            grep -q "^T46S flash backup" "$T/stick/t46s-backup/MANIFEST" && pass "T46S: backup in t46s-backup" || { cat "$T/out"; fail "T46S backup"; }
        refused install "No install files" "T46S: with only the T48S's kernel on the stick, install is refused" "$S0"
        # the T46S file name holding the T48S's kernel
        cp "$SET/T48S-kernel.uimg" "$T/stick/install/T46S-kernel.uimg"
        ( cd "$T/stick/install" && sha256sum T46S-kernel.uimg system.img > SHA256SUMS )
        refused install "Kernel file is not for this phone" "T46S: the T48S's kernel under the T46S's name is refused" "$S0"
        rm "$T/stick/install/T48S-kernel.uimg"
        cp "$SET/T46S-kernel.uimg" "$T/stick/install/"
        ( cd "$T/stick/install" && sha256sum T46S-kernel.uimg system.img > SHA256SUMS )
        echo "no environment (CRC wrong)" > "$T/param/layout"
        refused install "Flash layout not known" "T46S: install where the layout is not known is refused" "$S0"
        echo matches > "$T/param/layout"
        run install || { cat "$T/out"; fail "T46S install"; }
        last | grep -qx "done Installed" && pass "T46S: install finished" || fail "T46S install end"
        python3 "$HERE/tools/t48flash.py" kernelimage "$SET/T46S-kernel.uimg" "$T/k46.want" >/dev/null
        part 22675456 2621440 > "$T/k46"
        cmp "$T/k46" "$T/k46.want" && pass "T46S: kernel partition is exactly the T46S image, ECC bytes included" || fail "T46S kernel bytes"
        python3 "$HERE/tools/t48flash.py" nboot "$T/k46" "$SET/T46S-kernel.uimg" | tail -n 1 | sed 's/^/     /'
        for p in "0 22675456" "25296896 15728640" "119537664 14680064"; do
            set -- $p; [ "$(part $1 $2 | sha256sum)" = "$(dd if="$T/chip.keep" bs=2112 skip=$(($1 / 2048)) count=$(($2 / 2048)) 2>/dev/null | sha256sum)" ] || fail "T46S: something outside kernel and app changed"
        done
        pass "T46S: nothing outside the kernel and app partitions changed"
        echo none > "$T/bm/mode"
        run restore && last | grep -qx "done Restored" && [ "$(sum "$T/chip")" = "$S0" ] && pass "T46S: restore gives the backup back, byte for byte" || { cat "$T/out"; fail "T46S restore"; }
    else
        echo "skip T46S: no T46S-kernel.uimg in $SET"
    fi
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
