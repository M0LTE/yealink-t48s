#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# t48-flash.sh: the half of the installer that runs on the phone (issues 8, 29, 30;
# docs/install.md). tools/t48install.sh copies it into the phone's RAM with what
# it needs and runs one step at a time. It is not in the system image: an install
# and the way back are always driven from a PC that holds the backup.
#
#   t48-flash.sh state            what the phone is and holds, as NAME=VALUE lines
#   t48-flash.sh kernel           write kernel.dataoob to the kernel partition
#   t48-flash.sh app              turn the app partition into UBI with the two
#                                 system slots, write system.img to slot a, and
#                                 move /data into the new layout
#   t48-flash.sh quiet            let go of the flash, so that it can be dumped
#   t48-flash.sh unlock WHAT      open the driver's lock: kernel or app
#   t48-flash.sh lock             close the kernel lock again
#
# Every step can be run again after it was cut short: it looks at what is there.
# Beside this script, in its own folder: nandrestore, slots.sh, kernel.dataoob,
# system.img. T48_* variables are for the rehearsal on a PC (tools/test-install.sh).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PATH=$HERE:$PATH
PROC=${T48_PROC:-/proc}
LOCK=${T48_LOCK:-/sys/module/t48s_nand/parameters/lock}
PARAM=${T48_PARAM:-/sys/module/t48s_nand/parameters}
LOWER=${T48_LOWER:-539}   # blocks of app to erase: all but the 60 that have held /data (599 on a phone that never had ours)
SPARE=8                  # blocks left unallocated for UBI to move data into
. "$HERE/slots.sh"
say() { echo "install: $*"; }
die() { echo "install: STOPPED: $*" >&2; exit 1; }
mtd() { [ -n "$(eval echo \$T48_MTD_$1)" ] && eval echo \$T48_MTD_$1 || sed -n "s/^mtd\([0-9]*\):.*\"$1\"\$/\1/p" "$PROC/mtd"; }
ubi() { echo "$T48_UBISYS/ubi$T48_UBI"; }
attached() { [ -e "$(ubi)" ]; }
# UBI at the start of app: the new layout is there (the first stage's test)
app_is_ubi() {
    for b in 0 1 2 3 4 5 6 7; do
        [ "$(dd if=/dev/mtd"$(mtd t48app)" bs=4 skip=$((b * 32768)) count=1 2>/dev/null)" = "UBI#" ] && return 0
    done
    return 1
}
# every file's checksum and every other entry's kind, owner and mode, in name order
listing() {
    ( cd "$1" && find . | sort | while read -r f; do
        if [ -f "$f" ] && [ ! -L "$f" ]; then echo "$(sha256sum < "$f" | cut -d' ' -f1) $(stat -c '%a %u %g' "$f") $f"
        elif [ -L "$f" ]; then echo "link $(readlink "$f") $f"
        else echo "$(stat -c '%F %a %u %g' "$f") $f"; fi
    done )
}
detach() {
    attached || return 0
    if grep -q "^ubi$T48_UBI:data /data " "$PROC/mounts" 2>/dev/null; then
        # the running system: /data carries on in RAM with what it holds now
        t48-data release || die "/data is in use and could not be moved to RAM"
    fi
    attached || return 0
    for m in $(sed -n "s/^ubi$T48_UBI[:_][^ ]* \([^ ]*\) .*/\1/p" "$PROC/mounts"); do umount "$m"; done
    ubidetach -d "$T48_UBI" /dev/ubi_ctrl || die "UBI is in use"
}

case "$1" in
state)
    echo "kernel=$(uname -r)"
    echo "selftest=$(cat "$PARAM/selftest" 2>/dev/null)"
    echo "lock=$(cat "$LOCK" 2>/dev/null | tr ' ' ,)"
    echo "refused=$(cat "$PARAM/refused" 2>/dev/null)"
    echo "write_errors=$(cat "$PARAM/write_errors" 2>/dev/null)"
    echo "mtd_kernel=$(mtd t48kernel)"
    echo "mtd_app=$(mtd t48app)"
    echo "boot=$(tr ' ' '\n' < "$PROC/cmdline" | sed -n 's/^t48boot=//p')"
    echo "bootmode=$(cat /sys/kernel/t48s_bootmode/mode 2>/dev/null)"
    echo "hostname=$(hostname)"
    echo "data=$(grep -q "^ubi$T48_UBI:data /data " "$PROC/mounts" && echo flash || echo ram)"
    app_is_ubi && echo "app=ubi" || echo "app=other" ;;
unlock)
    case "$2" in kernel|app) echo "open-$2" > "$LOCK" ;; *) die "unlock kernel or app" ;; esac
    say "lock: $(cat "$LOCK")" ;;
lock)
    echo close-kernel > "$LOCK"
    say "lock: $(cat "$LOCK")" ;;
kernel)
    K=$(mtd t48kernel); [ -n "$K" ] || die "this kernel cannot write the kernel partition"
    [ -f "$HERE/kernel.dataoob" ] || die "no kernel.dataoob"
    say "kernel partition: erasing 20 blocks and writing"
    nandrestore "/dev/mtd$K" < "$HERE/kernel.dataoob" || die "the kernel partition is NOT complete. Do not restart the phone: run this step again"
    nandrestore -c "/dev/mtd$K" < "$HERE/kernel.dataoob" || die "the kernel partition does not read back. Do not restart the phone: run this step again"
    say "kernel partition written and read back, every byte" ;;
app)
    A=$(mtd t48app); [ -n "$A" ] || die "this kernel cannot write the app partition"
    [ -f "$HERE/system.img" ] || die "no system.img"
    [ "$(head -c 4 "$HERE/system.img")" = hsqs ] || die "system.img is not a system image"
    sum=$(sha256sum < "$HERE/system.img" | cut -d' ' -f1)
    if ! app_is_ubi; then
        detach
        say "app partition: erasing $LOWER blocks of Yealink's"
        nandrestore -e $LOWER "/dev/mtd$A" || die "erase failed; run this step again"
    fi
    if attached && [ "$(cat "$(ubi)/mtd_num")" != "$A" ]; then detach; fi
    attached || ubiattach -m "$A" -d "$T48_UBI" /dev/ubi_ctrl >/dev/null || die "UBI does not attach to the app partition"
    leb=$(cat "$(ubi)/eraseblock_size")
    say "UBI on the whole app partition: $(cat "$(ubi)/total_eraseblocks") blocks, $(cat "$(ubi)/bad_peb_count") bad"
    # the slots and the note; made once
    for v in system-a system-b; do
        slot_vol $v >/dev/null || ubimkvol "/dev/ubi$T48_UBI" -N $v -t static -s $((T48_SLOT_LEBS * leb)) >/dev/null || die "no room for $v"
    done
    for v in bootstate0 bootstate1; do
        slot_vol $v >/dev/null || ubimkvol "/dev/ubi$T48_UBI" -N $v -t static -s "$leb" >/dev/null || die "no room for $v"
    done
    # the system image, into slot a
    if [ "$(slot_sum a)" != "$sum" ]; then
        say "slot a: writing the system image ($(wc -c < "$HERE/system.img") bytes)"
        ubiupdatevol "$(slot_vol system-a)" "$HERE/system.img" || die "slot a did not take the image; run this step again"
    fi
    [ "$(slot_sum a)" = "$sum" ] || die "slot a does not read back as the system image"
    say "slot a holds the system image, read back: $sum"
    state_read
    if [ "$S_VALID" = 0 ] || [ "$S_ACTIVE" != a ] || [ "$S_TRIAL" != - ]; then
        state_write a - 0 || die "the boot note did not take"
    fi
    say "boot note: active=$S_ACTIVE trial=$S_TRIAL"
    # /data: into a volume that has the rest of the room. The old volume stays as
    # it is until the new one holds the same files; then the two change names in
    # one step, and only then is the old one removed.
    mkdir -p "$HERE/old" "$HERE/new"
    if slot_vol data-old >/dev/null && slot_vol data >/dev/null; then
        ubirmvol "/dev/ubi$T48_UBI" -N data-old            # cut short after the change of names
    fi
    if slot_vol data-new >/dev/null; then
        ubirmvol "/dev/ubi$T48_UBI" -N data-new            # cut short during the copy: start it again
    fi
    avail=$(cat "$(ubi)/avail_eraseblocks")
    if ! slot_vol data >/dev/null; then
        ubimkvol "/dev/ubi$T48_UBI" -N data -s $(( (avail - SPARE) * leb )) >/dev/null || die "no room for data"
        # an empty volume has no filesystem until it has been mounted for writing once
        mount -t ubifs "ubi$T48_UBI:data" "$HERE/new" && umount "$HERE/new" || die "the new /data does not mount"
        say "/data: a new, empty volume (there was none)"
    elif [ $((avail - SPARE)) -gt "$(cat "$T48_UBISYS/$(basename "$(slot_vol data)")/reserved_ebs")" ]; then
        # only ever into a larger volume: after the move the room left over is
        # smaller than /data, and a second run finds nothing to do
        ubimkvol "/dev/ubi$T48_UBI" -N data-new -s $(( (avail - SPARE) * leb )) >/dev/null || die "no room for the new data volume"
        mount -t ubifs -o ro "ubi$T48_UBI:data" "$HERE/old" || die "the old /data does not mount"
        mount -t ubifs "ubi$T48_UBI:data-new" "$HERE/new" || die "the new /data does not mount"
        cp -a "$HERE/old/." "$HERE/new/" && sync || die "copying /data failed; the old one is untouched"
        listing "$HERE/old" > "$HERE/old.list"; listing "$HERE/new" > "$HERE/new.list"
        cmp -s "$HERE/old.list" "$HERE/new.list" || die "the copy of /data differs from the original; the old one is untouched"
        n=$(wc -l < "$HERE/new.list")
        umount "$HERE/old"; umount "$HERE/new"
        ubirename "/dev/ubi$T48_UBI" data data-old data-new data || die "the change of names failed; the old /data is untouched"
        ubirmvol "/dev/ubi$T48_UBI" -N data-old
        say "/data moved to its new volume: $n files and folders, every one compared"
    else
        say "/data is already in its new volume"
    fi
    mount -t ubifs -o ro "ubi$T48_UBI:data" "$HERE/new" || die "/data does not mount"
    say "/data: $(find "$HERE/new" -type f | wc -l) files, $(df -k "$HERE/new" | tail -n 1 | awk '{print $3 " KB used of " $2}')"
    umount "$HERE/new"
    for d in "$(ubi)"_*; do say "volume $(cat "$d/name"): $(cat "$d/reserved_ebs") blocks"; done
    say "app partition done" ;;
quiet)
    detach
    say "flash released" ;;
*)  sed -n '8,17p' "$0" | sed 's/^# //' >&2; exit 2 ;;
esac
