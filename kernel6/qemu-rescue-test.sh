#!/bin/sh
# The first stage's way out when nothing starts (issue 32, docs/install.md), on
# a PC: build the kernel for QEMU's versatilepb machine with the command line of
# an installed phone (dts/qemu-versatilepb-test-flash.dts) and the first stage
# alone. The machine has no flash and no keypad, so the first stage must go
# through the rescue menu's "nothing starts" path, take its default without a
# key, and stay up as rescue with a shell on the serial port. It proves the
# first stage's script and t48-keys run as built; nothing about the phone's
# flash or keys (tools/test-install.sh and the menu on the phone do that).
#   ROOTFS=firststage.cpio sh kernel6/qemu-rescue-test.sh WORKDIR   (needs qemu-system-arm)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-.}
DTS=qemu-versatilepb-test-flash DEBUG=0 NAND=0 OUT=zImage-qemu-rescue sh "$HERE/build.sh" "$WORK"
LOG="$WORK/qemu-rescue-test.log"
( sleep 45; echo; echo 'hostname; t48-keys held x; echo "keys rc=$?"; ls /lib; echo QEMU-RESCUE-DONE'; sleep 8 ) |
    timeout 90 qemu-system-arm -M versatilepb -cpu arm1176 -m 128 -nographic -no-reboot \
        -device loader,file="$WORK/zImage-qemu-rescue",addr=0x00408000,cpu-num=0,force-raw=on > "$LOG" 2>&1 || true
fail() { tail -n 40 "$LOG"; echo "qemu-rescue-test: FAILED: $1" >&2; exit 1; }
grep -a -q "t48 boot: RESCUE: This kernel has no flash." "$LOG" || fail "the first stage did not end in rescue"
grep -a -q "QEMU-RESCUE-DONE" "$LOG" || fail "no rescue shell on the serial port"
grep -a -q "t48s-rescue" "$LOG" || fail "not the rescue system"
grep -a -q "keys rc=2" "$LOG" || fail "t48-keys did not run"
grep -a "t48 boot:\|keys rc" "$LOG" | tail -n 6
echo "qemu-rescue-test: passed"
