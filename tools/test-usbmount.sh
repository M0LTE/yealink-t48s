#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-usbmount.sh: the phone's t48-usbmount on any PC, no root (docs/apps.md).
# Sticks are folders standing in for sysfs and /dev, and mount, umount, blkid and
# mountpoint are stand-ins that keep a mounts file. What it shows: the names and
# options, partitions and a whole disk, a stick that is not FAT, one mounted by
# hand, taking a stick out (busy or not, and an event that comes before sysfs
# lets go), and the installer's hold and free, with an app holding a file
# open and with the lock taken.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }

mkdir -p "$T/sys/block" "$T/sys/class/block" "$T/dev" "$T/proc" "$T/bin" "$T/fs" "$T/busy"
: > "$T/proc/mounts"
# a stick: disk name, filesystem, label, then partition numbers (none: a whole disk)
stick() {
    d=$1; fs=$2; label=$3; shift 3
    mkdir -p "$T/sys/block/$d" "$T/sys/class/block/$d"; : > "$T/dev/$d"
    [ $# = 0 ] && { echo "$fs" > "$T/fs/$d"; [ -z "$label" ] || echo "$label" > "$T/fs/$d.label"; }
    for p in "$@"; do
        mkdir -p "$T/sys/block/$d/$d$p" "$T/sys/class/block/$d$p"; : > "$T/sys/block/$d/$d$p/partition"
        : > "$T/dev/$d$p"; echo "$fs" > "$T/fs/$d$p"
        [ -z "$label" ] || echo "$label" > "$T/fs/$d$p.label"
    done
}
pull() { rm -rf "$T/sys/block/$1" "$T/sys/class/block/$1"*; }

# mount -t TYPE -o OPTS DEV DIR: right type only, utf8 only when the kernel has it
cat > "$T/bin/mount" <<'EOS'
#!/bin/sh
t=$2; o=$4; d=$5; m=$6; n=${d##*/}
[ "$(cat "$STUB/fs/$n" 2>/dev/null)" = "$t" ] || exit 1
case ,$o, in *,utf8,*) [ -z "$STUB_NOUTF8" ] || exit 1 ;; esac
grep -q "^$d " "$STUB/proc/mounts" && exit 1
echo "$d $m $t $o 0 0" >> "$STUB/proc/mounts"
EOS
cat > "$T/bin/umount" <<'EOS'
#!/bin/sh
lazy=; [ "$1" = -l ] && { lazy=1; shift; }
grep -q " $1 " "$STUB/proc/mounts" || exit 1
[ -n "$lazy" ] || [ ! -e "$STUB/busy/${1##*/}" ] || exit 1
grep -v " $1 " "$STUB/proc/mounts" > "$STUB/m"; cat "$STUB/m" > "$STUB/proc/mounts"
EOS
cat > "$T/bin/blkid" <<'EOS'
#!/bin/sh
n=${1##*/}
if [ -f "$STUB/fs/$n.label" ]; then echo "$1: LABEL=\"$(cat "$STUB/fs/$n.label")\" UUID=\"16C1-75C2\""; else echo "$1: UUID=\"16C1-75C2\""; fi
EOS
cat > "$T/bin/mountpoint" <<'EOS'
#!/bin/sh
grep -q " $2 " "$STUB/proc/mounts"
EOS
chmod +x "$T/bin/"*
export PATH="$T/bin:$PATH" STUB="$T" T48_SYS="$T/sys" T48_PROC="$T/proc" T48_DEV="$T/dev" T48_MEDIA="$T/media" T48_USBRUN="$T/run"
U="sh $HERE/buildroot/board/t48s/rootfs-overlay/usr/bin/t48-usbmount"
mounted() { awk -v d="$T/dev/$1" '$1 == d { print $2 }' "$T/proc/mounts"; }
opts() { awk -v d="$T/dev/$1" '$1 == d { print $4 }' "$T/proc/mounts"; }

stick sda vfat "NEW VOLUME" 1
$U sync >/dev/null
[ "$(mounted sda1)" = "$T/media/NEW_VOLUME" ] && pass "a FAT partition goes on /run/media/LABEL" || fail "label name: $(mounted sda1)"
case ,$(opts sda1), in *,ro,*nosuid,nodev,noexec,*fmask=0133,dmask=0022,utf8,) pass "read-only, no programs or devices, files 644, UTF-8 names" ;; *) fail "options $(opts sda1)" ;; esac
[ -z "$(mounted sda)" ] && pass "a partitioned disk itself is not tried" || fail "whole disk mounted"

stick sdb exfat ""
$U sync >/dev/null
[ "$(mounted sdb)" = "$T/media/sdb" ] && pass "exFAT on a disk with no partition table, no label: /run/media/sdb" || fail "whole disk: $(mounted sdb)"

stick sdc vfat "NEW VOLUME" 1
STUB_NOUTF8=1 $U sync >/dev/null
[ "$(mounted sdc1)" = "$T/media/NEW_VOLUME-sdc1" ] && pass "a label already in use gets the device added" || fail "same label: $(mounted sdc1)"
case ,$(opts sdc1), in *,utf8,*) fail "utf8 on a kernel without it" ;; *) pass "a kernel without utf8 still mounts it" ;; esac

stick sdd ntfs "WIN" 1
stick sde vfat "NO NAME" 1
$U sync >/dev/null
[ -z "$(mounted sdd1)" ] && [ ! -e "$T/media/WIN" ] && pass "not FAT or exFAT: left alone, no folder left behind" || fail "ntfs"
[ "$(mounted sde1)" = "$T/media/sde1" ] && pass "FAT's empty label (NO NAME) gives the device name" || fail "NO NAME: $(mounted sde1)"

stick sdf vfat "HAND" 1
echo "$T/dev/sdf1 /run/usb vfat ro 0 0" >> "$T/proc/mounts"
$U sync >/dev/null
[ "$(mounted sdf1)" = /run/usb ] && pass "a stick mounted by hand is left alone" || fail "by hand"

pull sda
$U sync >/dev/null
[ -z "$(mounted sda1)" ] && [ ! -e "$T/media/NEW_VOLUME" ] && pass "taken out: unmounted, folder gone" || fail "pull"

: > "$T/busy/NEW_VOLUME-sdc1"
pull sdc
$U sync | grep -q "lazily" && [ -z "$(mounted sdc1)" ] && pass "taken out while an app reads it: let go lazily" || fail "busy pull"

# the remove event can come while sysfs still shows the device
SUBSYSTEM=block DEVNAME=sdb ACTION=remove $U event >/dev/null
[ -z "$(mounted sdb)" ] && pass "a remove event unmounts at once, and the sync does not mount it again" || fail "remove event"
SUBSYSTEM=block DEVNAME=sdb ACTION=add $U event >/dev/null
[ -n "$(mounted sdb)" ] && pass "an add event mounts" || fail "add event"
pull sdb; SUBSYSTEM=input DEVNAME=input/event0 ACTION=add $U event >/dev/null
[ -n "$(mounted sdb)" ] && pass "other subsystems' events do nothing" || fail "input event"
$U sync >/dev/null

# the installer: hold, then free
$U hold sde1 >/dev/null
[ -z "$(mounted sde1)" ] && pass "hold unmounts" || fail "hold"
$U sync >/dev/null
[ -z "$(mounted sde1)" ] && pass "a held stick is not mounted again" || fail "held remounted"
$U free sde1 >/dev/null
[ "$(mounted sde1)" = "$T/media/sde1" ] && pass "free mounts it again" || fail "free"
$U hold sde1 >/dev/null; pull sde; $U sync >/dev/null
[ ! -e "$T/run/hold-sde1" ] && pass "a hold ends when the stick is taken out" || fail "hold left"
stick sde vfat "NO NAME" 1; $U sync >/dev/null
[ -n "$(mounted sde1)" ] && pass "the next stick in is mounted" || fail "after hold"

stick sdg vfat "$(printf 'TWO\nLINES')" 1; $U sync >/dev/null
[ "$(mounted sdg1)" = "$T/media/sdg1" ] && pass "a label holding a newline gives the device name" || fail "newline label: $(mounted sdg1)"
: > "$T/busy/sdg1"
if $U hold sdg1 2>/dev/null; then fail "held while an app had it open"; fi
[ -n "$(mounted sdg1)" ] && [ ! -e "$T/run/hold-sdg1" ] && pass "hold gives up after 5 seconds while an app has it open, and leaves it mounted" || fail "busy hold"
rm "$T/busy/sdg1"

flock "$T/run/lock" sleep 9 & L=$!
sleep 1
s=$(date +%s)
if $U hold sde1 2>/dev/null; then fail "held without the lock"; fi
[ $(($(date +%s) - s)) -le 6 ] && [ -n "$(mounted sde1)" ] && pass "hold gives up after 5 seconds when the lock is not free" || fail "lock wait"
T48_USBLOCKED=1 $U hold sde1 >/dev/null && [ -z "$(mounted sde1)" ] && pass "the installer, holding the lock, can hold" || fail "hold under the caller's lock"
wait $L
echo "all usbmount tests passed"
