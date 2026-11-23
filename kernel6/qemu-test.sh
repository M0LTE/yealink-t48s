#!/bin/sh
# Smoke test on a PC: build the T48S kernel configuration for QEMU's versatilepb
# machine (ARM1176, see dts/qemu-versatilepb-test.dts) and boot it to the shell
# prompt. It proves the configuration, the device-tree-only boot, the interrupt
# controller, timer and serial drivers, and that the root filesystem runs on this
# kernel. It proves nothing about the phone's own hardware.
#   ROOTFS=rootfs.cpio.gz sh kernel6/qemu-test.sh WORKDIR     (needs qemu-system-arm)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-.}
DTS=qemu-versatilepb-test DEBUG=0 OUT=zImage-qemu sh "$HERE/build.sh" "$WORK"
LOG="$WORK/qemu-test.log"
( sleep 25; echo 'uname -a; cat /proc/interrupts; grep MemTotal /proc/meminfo; sleep 2; date; echo QEMU-TEST-DONE'; sleep 8 ) |
    timeout 60 qemu-system-arm -M versatilepb -cpu arm1176 -m 128 -nographic -no-reboot \
        -kernel "$WORK/zImage-qemu" > "$LOG" 2>&1 || true
grep -a -q "QEMU-TEST-DONE" "$LOG" && grep -a -q "Linux t48s" "$LOG" || { tail -n 40 "$LOG"; echo "qemu-test: FAILED" >&2; exit 1; }
grep -a "Linux t48s\|MemTotal\|timer\|uart" "$LOG" | tail -n 8
echo "qemu-test: passed"
