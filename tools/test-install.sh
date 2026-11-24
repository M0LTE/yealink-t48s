#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-install.sh: rehearse the install, an update, a failed update and the way
# back, without the phone (issues 8, 29, 30, 31; docs/install.md).
#
#   test-install.sh bytes SET BACKUP
#       Any PC. A file stands in for the whole flash, filled from the backup, and
#       nandrestore treats it as the kernel driver treats the chip (only kernel
#       and app can be written, a page must be erased first, the ECC bytes are
#       worked out). Checks the exact bytes: the kernel partition after the
#       install, U-Boot's read of it, every refusal, and that the way back leaves
#       the flash identical to the backup.
#
#   test-install.sh ubi SET
#       Linux, as root, with the nandsim, ubi and ubifs modules and the UBI
#       tools (mtd-utils or busybox). The kernel's simulated flash chip, the
#       phone's size, and the kernel's real UBI. Runs the phone's own scripts:
#       the installer's app step around a /data that is already there, then
#       starts, an update, its confirmation, an update that fails, a damaged
#       slot, a boot note cut short, and the way back.
#       What it cannot show is the phone's ECC bytes: nandsim has its own.
#
# SET is an install set (tools/t48install.sh set).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
MODE=$1; SET=$2
[ -f "$SET/system.img" ] && [ -f "$SET/kernel.dataoob" ] || { sed -n '5,24p' "$0" | sed 's/^# //' >&2; exit 2; }
SET=$(cd "$SET" && pwd)
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }
sum() { sha256sum < "$1" | cut -d' ' -f1; }

case "$MODE" in
bytes)
    BACKUP=$3; [ -f "$BACKUP/mtd10-app.dataoob" ] || exit 2
    T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
    cc -O2 -Wall -o "$T/nr" "$HERE/native/nandrestore.c"
    NR="$T/nr -f $T/chip"
    order="mtd0-ubl mtd1-uboot mtd2-env mtd3-userenv mtd4-logo mtd5-smkern mtd6-smrfs mtd7-kernel mtd8-rfs mtd10-app mtd11-cfg mtd12-data"
    for p in $order; do cat "$BACKUP/$p.dataoob"; done > "$T/chip"
    cp "$T/chip" "$T/stock"
    S0=$(sum "$T/stock")
    # a partition of the chip file, by flash address and size
    part() { dd if="$T/chip" bs=2112 skip=$(($1 / 2048)) count=$(($2 / 2048)) 2>/dev/null; }
    # the chip file with one partition cut out, for "nothing else changed"
    others() { { dd if="$1" bs=2112 count=$(($2 / 2048)) 2>/dev/null; dd if="$1" bs=2112 skip=$((($2 + $3) / 2048)) 2>/dev/null; } | sha256sum; }
    K=$((0x015a0000)); KS=$((0x00280000)); A=$((0x02720000)); AS=$((0x04ae0000))

    $NR -p kernel -c < "$BACKUP/mtd7-kernel.dataoob" >/dev/null && pass "the chip file starts as the backup" || fail "the chip file starts as the backup"
    # 1. nothing that is not ours can be erased or written
    for p in ubl uboot env userenv logo smkern smrfs rfs cfg data; do
        if $NR -p $p -e 1 2>/dev/null; then fail "$p was erased"; fi
        f=$(ls "$BACKUP"/mtd*-$p.dataoob)
        if $NR -p $p < "$f" 2>/dev/null; then fail "$p was written"; fi
    done
    [ "$(sum "$T/chip")" = "$S0" ] && pass "ubl uboot env userenv logo smkern smrfs rfs cfg data: erase and write refused, chip unchanged" || fail "ubl uboot env userenv logo smkern smrfs rfs cfg data: erase and write refused, chip unchanged"
    # 2. the kernel step
    $NR -p kernel < "$SET/kernel.dataoob" | sed 's/^/     /'
    part $K $KS > "$T/k"
    cmp "$T/k" "$SET/kernel.dataoob" && pass "kernel partition: exactly the image the PC worked out, ECC bytes included" || fail "kernel partition: exactly the image the PC worked out, ECC bytes included"
    python3 "$HERE/tools/nandcheck.py" ecc "$T/k" | grep -q " 0 differ" && pass "kernel partition: every written step has the right ECC" || fail "kernel partition: every written step has the right ECC"
    python3 "$HERE/tools/t48flash.py" nboot "$T/k" "$SET/T48S-kernel.uimg" | tail -n 1 | sed 's/^/     /'
    [ "$(others "$T/chip" $K $KS)" = "$(others "$T/stock" $K $KS)" ] && pass "nothing outside the kernel partition changed" || fail "nothing outside the kernel partition changed"
    # the other way to write it (t48-update kernel): the plain file
    cp "$T/chip" "$T/chip-a"
    $NR -p kernel < "$BACKUP/mtd7-kernel.dataoob" >/dev/null
    $NR -p kernel -b < "$SET/T48S-kernel.uimg" | sed 's/^/     /'
    cmp "$T/chip" "$T/chip-a" && pass "writing the plain kernel file gives the same bytes" || fail "writing the plain kernel file gives the same bytes"
    # 3. the app step's erase: Yealink's 539 blocks, and not the 60 with /data
    $NR -p app -e 539 | sed 's/^/     /'
    part $A $((539 * 131072)) | tr -d '\377' | head -c 1 | wc -c | grep -q '^0$' && pass "app blocks 0 to 538 read erased" || fail "app blocks 0 to 538 read erased"
    part $((A + 539 * 131072)) $((60 * 131072)) > "$T/d"
    tail -c $((60 * 64 * 2112)) "$BACKUP/mtd10-app.dataoob" | cmp - "$T/d" && pass "app blocks 539 to 598 (where /data is) untouched" || fail "app blocks 539 to 598 (where /data is) untouched"
    # 4. the way back: app, then kernel
    $NR -p app < "$BACKUP/mtd10-app.dataoob" | sed 's/^/     /'
    $NR -p kernel < "$BACKUP/mtd7-kernel.dataoob" | sed 's/^/     /'
    [ "$(sum "$T/chip")" = "$S0" ] && pass "after the way back the whole flash is the backup, byte for byte: $S0" || fail "after the way back the whole flash is the backup, byte for byte: $S0"
    echo "ALL PASSED (bytes)" ;;

ubi)
    [ "$(id -u)" = 0 ] || { echo "needs root" >&2; exit 1; }
    T=$(mktemp -d)
    U=7                                          # a UBI device number nothing else uses
    export T48_UBI=$U T48_TMP=$T T48_RUN=$T/run T48_NO_SV=1 T48_UBISYS=/sys/class/ubi
    export T48_SLOTS=$HERE/buildroot/board/t48s/rootfs-overlay/usr/lib/t48/slots.sh
    mkdir -p "$T/run" "$T/inst" "$T/mnt"
    cleanup() {
        for m in $(sed -n "s/^ubi$U[:_][^ ]* \([^ ]*\) .*/\1/p" /proc/mounts); do umount "$m" 2>/dev/null || true; done
        ubidetach -d $U /dev/ubi_ctrl 2>/dev/null || true
        rmmod nandsim 2>/dev/null || true
        rm -rf "$T"
    }
    trap cleanup EXIT
    modprobe ubi; modprobe ubifs
    # as the phone's kernel is built: two blocks in 1024 kept back for bad ones
    printf '#!/bin/sh\nexec %s -b 2 "$@"\n' "$(command -v ubiattach)" > "$T/inst/ubiattach"
    chmod +x "$T/inst/ubiattach"
    PATH=$T/inst:$PATH
    # the phone's chip: 128 MB, 2048-byte pages, 64 spare bytes, 128 KB blocks
    SIM="first_id_byte=0xef second_id_byte=0xf1 third_id_byte=0x00 fourth_id_byte=0x95"
    simpart() { sed -n "s/^mtd\([0-9]*\): $1 00020000 \"NAND simulator.*/\1/p" /proc/mtd | head -n 1; }
    # today's /data: UBI on 60 blocks of their own, then lifted off the chip raw
    modprobe nandsim $SIM parts=313,539,60
    DAT=$(simpart 00780000); [ -n "$DAT" ] || fail "no simulated chip of the phone's size"
    ubiattach -m $DAT -d $U /dev/ubi_ctrl >/dev/null
    ubimkvol /dev/ubi$U -N data -s $((50 * $(cat /sys/class/ubi/ubi$U/eraseblock_size))) >/dev/null
    mount -t ubifs ubi$U:data "$T/mnt"
    mkdir -p "$T/mnt/config" "$T/mnt/access/ssh" "$T/mnt/apps/clock"
    echo "user=t48s" > "$T/mnt/config/sip"; chmod 600 "$T/mnt/config/sip"
    head -c 900000 /dev/urandom > "$T/mnt/apps/clock/big"
    echo key > "$T/mnt/access/ssh/authorized_keys"; ln -s ../config "$T/mnt/apps/link"
    ( cd "$T/mnt" && find . -type f | sort | xargs sha256sum ) > "$T/data.before"
    umount "$T/mnt"; ubidetach -d $U /dev/ubi_ctrl
    nanddump -n -o -q -f "$T/data.raw" /dev/mtd$DAT
    rmmod nandsim
    # the chip again, with app as one partition as the installer sees it: Yealink's
    # data in its first 539 blocks, those 60 blocks of /data at its end
    modprobe nandsim $SIM parts=313,599
    APP=$(simpart 04ae0000); [ -n "$APP" ] || fail "no app partition on the simulated chip"
    head -c $((539 * 131072)) /dev/urandom > "$T/yealink"
    nandwrite -q /dev/mtd$APP "$T/yealink"
    nandwrite -q -n -o -s $((539 * 131072)) /dev/mtd$APP "$T/data.raw"
    export T48_MTD_t48app=$APP T48_MTD_t48kernel=none T48_LOCK=$T/lock T48_PARAM=$T
    pass "simulated chip set up as the phone is today: app is mtd$APP, $(wc -l < "$T/data.before") files in /data in its last 60 blocks"

    # the installer's own script and tools, as the PC copies them to the phone
    cc -O2 -o "$T/inst/nandrestore" "$HERE/native/nandrestore.c"
    cp "$HERE/tools/phone/t48-flash.sh" "$T48_SLOTS" "$SET/system.img" "$T/inst/"
    sh "$T/inst/t48-flash.sh" unlock app | sed 's/^/     /'
    sh "$T/inst/t48-flash.sh" app | sed 's/^/     /'
    . "$T48_SLOTS"
    [ "$(slot_sum a)" = "$(sum "$SET/system.img")" ] && pass "install: slot a holds the system image" || fail "install: slot a holds the system image"
    mount -t ubifs -o ro ubi$U:data "$T/mnt"
    ( cd "$T/mnt" && find . -type f | sort | xargs sha256sum ) | cmp - "$T/data.before" && pass "install: every file of /data is there, unchanged" || fail "install: every file of /data is there, unchanged"
    umount "$T/mnt"
    # run again: nothing to do, nothing harmed
    vols() { for d in /sys/class/ubi/ubi${U}_*; do echo "$(cat $d/name) $(cat $d/reserved_ebs)"; done | sort; }
    vols > "$T/vols.1"
    sh "$T/inst/t48-flash.sh" app > "$T/second.log"
    vols | cmp -s - "$T/vols.1" && grep -q "already in its new volume" "$T/second.log" && pass "install: a second run changes nothing" || fail "a second run changed the volumes" || fail "install: a second run changes nothing"

    # one start of the phone, as the first stage does it; prints the slot started
    start() {
        ubidetach -d $U /dev/ubi_ctrl; ubiattach -m $APP -d $U /dev/ubi_ctrl >/dev/null
        slot_plan
        for s in $SLOT_ORDER; do
            if slot_load "$s" "$T/system.img" && unsquashfs -s "$T/system.img" >/dev/null 2>&1; then
                printf 'slot=%s\nnote=%s\n' "$s" "$SLOT_NOTE" > "$T/run/t48-slot"
                STARTED=$s; return 0
            fi
        done
        STARTED=rescue
    }
    UPD=$HERE/buildroot/board/t48s/rootfs-overlay/usr/bin/t48-update
    mkimg() { mkdir -p "$T/root$1"; echo "$1" > "$T/root$1/version"; mksquashfs "$T/root$1" "$T/sys$1.img" -quiet -noappend >/dev/null; }
    mkimg 2; mkimg 3

    start; [ "$STARTED" = a ] && [ -z "$SLOT_NOTE" ] && pass "start 1: slot a" || fail "start 1: slot a"
    sh "$UPD" install "$T/sys2.img" "$(sum "$T/sys2.img")" | sed 's/^/     /'
    state_read; [ "$S_ACTIVE $S_TRIAL $S_TRIES" = "a b 1" ] && pass "update: written to slot b, on trial, one try" || fail "update: written to slot b, on trial, one try"
    start; [ "$STARTED" = b ] && [ "$SLOT_NOTE" = trial ] && pass "start 2: slot b, on its one try" || fail "start 2: slot b, on its one try"
    state_read; [ "$S_ACTIVE $S_TRIAL $S_TRIES" = "a b 0" ] && pass "the try was counted before the slot started" || fail "the try was counted before the slot started"
    if sh "$UPD" install "$T/sys3.img" 2>/dev/null; then fail "an update was accepted while the running slot was on trial"; fi
    pass "no second update while the running slot is still on trial"
    sh "$UPD" confirm | sed 's/^/     /'
    state_read; [ "$S_ACTIVE $S_TRIAL" = "b -" ] && pass "after a minute's healthy running: slot b is the active one" || fail "after a minute's healthy running: slot b is the active one"
    start; [ "$STARTED" = b ] && [ -z "$SLOT_NOTE" ] && pass "start 3: slot b" || fail "start 3: slot b"

    sh "$UPD" install "$T/sys3.img" >/dev/null
    start; [ "$STARTED" = a ] && [ "$SLOT_NOTE" = trial ] && pass "start 4: the next update, in slot a, on trial" || fail "start 4: the next update, in slot a, on trial"
    # ... and it hangs: no confirm, the watchdog resets the phone
    start; [ "$STARTED" = b ] && [ "$SLOT_NOTE" = "failed=a" ] && pass "start 5: slot a never confirmed, so back to slot b by itself" || fail "start 5: slot a never confirmed, so back to slot b by itself"
    state_read; [ "$S_ACTIVE $S_TRIAL" = "b -" ] && pass "the failed trial is dropped" || fail "the failed trial is dropped"
    start; [ "$STARTED" = b ] && [ -z "$SLOT_NOTE" ] && pass "start 6: slot b" || fail "start 6: slot b"

    # a slot that passes for an image but does not mount
    { printf hsqs; head -c 200000 /dev/urandom; } > "$T/bad.img"
    sh "$UPD" install "$T/bad.img" >/dev/null
    start; [ "$STARTED" = b ] && pass "start 7: a damaged image in slot a does not mount; slot b starts in the same boot" || fail "start 7: a damaged image in slot a does not mount; slot b starts in the same boot"
    start; [ "$STARTED" = b ] && [ "$SLOT_NOTE" = "failed=a" ] && pass "start 8: and the trial is dropped" || fail "start 8: and the trial is dropped"

    # an update cut short while the slot is being written: UBI marks the volume
    state_read
    v=$(slot_vol system-a)
    head -c 100000 /dev/urandom | ubiupdatevol "$v" -s 300000 - 2>/dev/null || true
    [ -z "$(slot_sum a)" ] && pass "a slot whose write was cut short cannot be read at all" || fail "a slot whose write was cut short cannot be read at all"
    start; [ "$STARTED" = b ] && pass "start 9: slot b; the half-written slot is never tried" || fail "start 9: slot b; the half-written slot is never tried"
    sh "$UPD" switch 2>/dev/null && fail "switch to an unreadable slot was accepted"
    pass "switching to an unreadable slot is refused"

    # the boot note cut short: the copy being written is lost, the other is in force
    state_read; seq=$S_SEQ
    ubiupdatevol "$(slot_vol bootstate$(( (seq + 1) % 2 )))" -t
    : > "$T/empty"; ubiupdatevol "$(slot_vol bootstate$(( (seq + 1) % 2 )))" "$T/empty" 2>/dev/null || true
    state_read; [ "$S_VALID" = 1 ] && [ "$S_SEQ" = "$seq" ] && [ "$S_ACTIVE" = b ] && pass "one copy of the boot note wiped: the other is in force" || fail "one copy of the boot note wiped: the other is in force"
    ubiupdatevol "$(slot_vol bootstate0)" -t; ubiupdatevol "$(slot_vol bootstate1)" -t
    start; [ "$STARTED" = b ] && pass "both copies wiped: slot a is the default, it is unreadable, slot b starts" || fail "both copies wiped: slot a is the default, it is unreadable, slot b starts"
    sh "$UPD" confirm >/dev/null
    state_read; [ "$S_VALID $S_ACTIVE $S_TRIAL" = "1 b -" ] && pass "a minute later the slot that is running is written down as the active one" || fail "self-heal"
    sh "$UPD" install "$T/sys3.img" >/dev/null
    state_read; [ "$S_ACTIVE $S_TRIAL $S_TRIES" = "b a 1" ] && pass "and the next update goes to slot a as usual" || fail "next update" || fail "the next update writes the note afresh ($S_ACTIVE active, $S_TRIAL on trial)"

    # through all of that, /data
    mount -t ubifs -o ro ubi$U:data "$T/mnt"
    ( cd "$T/mnt" && find . -type f | sort | xargs sha256sum ) | cmp - "$T/data.before" && pass "/data: every file still there, unchanged" || fail "/data: every file still there, unchanged"
    umount "$T/mnt"
    # what the PC would read back from the flash
    ubidetach -d $U /dev/ubi_ctrl
    cat /dev/mtd$APP > "$T/app.bin"
    [ -z "$T48_KEEP_DUMP" ] || cp "$T/app.bin" "$T48_KEEP_DUMP"
    python3 "$HERE/tools/t48flash.py" ubi "$T/app.bin" plain system-b="$T/sys2.img" > "$T/ubi.txt" || { cat "$T/ubi.txt"; fail "the flash, read as a PC would read it, is not what was written"; }
    sed 's/^/     /' "$T/ubi.txt"
    pass "read back from the raw flash on a PC: the volumes are there and slot b holds its image"

    # the way back begins by erasing all of app (the bytes are test-install.sh bytes)
    sh "$T/inst/t48-flash.sh" quiet >/dev/null
    "$T/inst/nandrestore" -e 599 /dev/mtd$APP | sed 's/^/     /'
    [ -z "$(tr -d '\377' < /dev/mtd$APP | head -c 1)" ] && pass "way back: app erased, nothing of ours left in it" || fail "app is not erased"
    echo "ALL PASSED (ubi)" ;;
*) sed -n '5,24p' "$0" | sed 's/^# //' >&2; exit 2 ;;
esac
