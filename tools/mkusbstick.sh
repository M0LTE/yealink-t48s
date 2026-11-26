#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# mkusbstick.sh /dev/sdX DIR: make a USB stick the phone's U-Boot can start our
# firmware from (issue #7, docs/live-image.md). ERASES THE WHOLE STICK.
#
# DIR is what "tools/mkliveimage.py usb DIR ..." wrote: T48S.rom, T48S.bin,
# T48S.rfs and README.txt (T46S.* for a T46S), usually with install/: an unpacked
# t48s-stick.zip or t46s-stick.zip. The stick gets what U-Boot 2.5.3.0 needs, as read from
# its code: an MBR partition table (not GPT, and not "no partition table"), one
# primary partition, FAT32 (FAT16 under 1 GB), the files in the top folder. Then
# it is read back the way U-Boot reads it (mkliveimage.py checkstick).
#
# Refuses anything that is not a whole USB disk, anything mounted, and anything
# over 64 GB unless BIG=1 (a guard against picking the wrong disk). YES=1 skips
# the question. Needs root, mkfs.vfat (dosfstools) and sfdisk or parted.
set -e
DEV=$1; DIR=$2
# the model the folder is for: T46S when it holds T46S.rom and no T48S.rom
[ -f "$DIR/T46S.rom" ] && [ ! -f "$DIR/T48S.rom" ] && MODEL=${MODEL:-T46S}
MODEL=${MODEL:-T48S}
HERE=$(cd "$(dirname "$0")" && pwd)
[ -n "$DEV" ] && [ -d "$DIR" ] || { echo "usage: $0 /dev/sdX DIR   (erases the stick)" >&2; exit 2; }
for f in $MODEL.rom $MODEL.bin $MODEL.rfs README.txt; do
    [ -f "$DIR/$f" ] || { echo "$DIR/$f is missing: run tools/mkliveimage.py usb $DIR ... first" >&2; exit 2; }
done
[ -b "$DEV" ] || { echo "$DEV is not a block device" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 2; }
command -v mkfs.vfat >/dev/null || { echo "mkfs.vfat is missing: install dosfstools" >&2; exit 2; }
command -v sfdisk >/dev/null || command -v parted >/dev/null || { echo "sfdisk or parted is needed (util-linux or parted)" >&2; exit 2; }
NAME=$(basename "$DEV"); SYS=/sys/block/$NAME
[ -d "$SYS" ] || { echo "$DEV is not a whole disk (give /dev/sdX, not /dev/sdX1)" >&2; exit 2; }
case "$(readlink -f "$SYS")" in
*/usb*) ;;
*) [ "$ANYDISK" = 1 ] || { echo "$DEV is not a USB disk; not touching it" >&2; exit 2; } ;;
esac
SECTORS=$(cat "$SYS/size")
[ "$SECTORS" -ge 65536 ] || { echo "$DEV is under 32 MB: too small" >&2; exit 2; }
[ "$SECTORS" -le 134217728 ] || [ "$BIG" = 1 ] || { echo "$DEV is over 64 GB: is it really the stick? BIG=1 to go on" >&2; exit 2; }
[ "$(cat "$SYS/queue/logical_block_size")" = 512 ] || { echo "$DEV does not have 512-byte sectors: U-Boot will not read it" >&2; exit 2; }
if grep -q "^$DEV" /proc/mounts; then echo "$DEV is mounted; unmount it first" >&2; exit 2; fi
echo "About to ERASE $DEV: $(cat "$SYS/device/vendor" 2>/dev/null) $(cat "$SYS/device/model" 2>/dev/null), $((SECTORS / 2048)) MB"
if [ "$YES" != 1 ]; then
    printf 'Type yes to go on: '; read -r a; [ "$a" = yes ] || { echo "nothing done"; exit 1; }
fi
# start clean: old filesystem and partition table signatures, the GPT copy at the end too
wipefs -a -q "$DEV" 2>/dev/null || true
dd if=/dev/zero of="$DEV" bs=1M count=4 conv=fsync 2>/dev/null
dd if=/dev/zero of="$DEV" bs=512 seek=$((SECTORS - 2048)) count=2048 conv=fsync 2>/dev/null || true
# one primary partition from 1 MB to the end, not flagged bootable: FAT32 (type 0c),
# or FAT16 (type 0e) on a stick under 1 GB, where FAT32 would not fit properly
if [ "$SECTORS" -lt 2097152 ]; then FAT=16; TYPE=e; PFS=fat16; else FAT=32; TYPE=c; PFS=fat32; fi
if command -v sfdisk >/dev/null; then
    printf 'label: dos\nstart=2048, type=%s\n' $TYPE | sfdisk -q "$DEV"
else
    parted -s "$DEV" mklabel msdos mkpart primary $PFS 1MiB 100%
fi
sync; partprobe "$DEV" 2>/dev/null || blockdev --rereadpt "$DEV" 2>/dev/null || true
case "$NAME" in *[0-9]) PART=${DEV}p1 ;; *) PART=${DEV}1 ;; esac
n=0; while [ ! -b "$PART" ] && [ $n -lt 20 ]; do sleep 0.5; n=$((n + 1)); done
[ -b "$PART" ] || { echo "$PART did not appear" >&2; exit 1; }
mkfs.vfat -F $FAT -n "$MODEL" "$PART" >/dev/null
# the three boot files first, on a fresh filesystem: each in one piece, at the start
M=$(mktemp -d)
mount -t vfat "$PART" "$M"
for f in $MODEL.rom $MODEL.bin $MODEL.rfs README.txt; do cp "$DIR/$f" "$M/$f"; done
# after the files U-Boot reads, so that those stay at the start of the stick: what
# the on-phone installer wants (docs/installer.md)
[ ! -d "$DIR/install" ] || cp -r "$DIR/install" "$M/install"
sync; umount "$M"; rmdir "$M"; sync
python3 "$HERE/mkliveimage.py" checkstick "$DEV" "$DIR" model="$MODEL"
echo "The stick is ready. It is safe to pull out."
