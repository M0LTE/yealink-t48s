#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Smoke test on a PC: build the T48S kernel configuration for QEMU's versatilepb
# machine (ARM1176, see dts/qemu-versatilepb-test.dts) and boot it to the shell
# prompt. It proves the configuration, the device-tree-only boot, the interrupt
# controller, timer and serial drivers, and that the root filesystem runs on this
# kernel. The image is put where the phone's loader puts it (0x00408000, nothing
# in the registers) and the first 4 MB are not declared as RAM, as on the phone,
# so the kernel has to place itself at 0x00400000 from the appended device tree
# (patches/0003). It proves nothing about the phone's own hardware.
#   ROOTFS=rootfs.cpio.gz sh kernel6/qemu-test.sh WORKDIR     (needs qemu-system-arm)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-.}
DTS=qemu-versatilepb-test DEBUG=0 OUT=zImage-qemu sh "$HERE/build.sh" "$WORK"
LOG="$WORK/qemu-test.log"
( sleep 25; echo 'uname -a; cat /proc/interrupts; grep MemTotal /proc/meminfo; sleep 2; date; echo QEMU-TEST-DONE'; sleep 8 ) |
    timeout 60 qemu-system-arm -M versatilepb -cpu arm1176 -m 128 -nographic -no-reboot \
        -device loader,file="$WORK/zImage-qemu",addr=0x00408000,cpu-num=0,force-raw=on > "$LOG" 2>&1 || true
grep -a -q "QEMU-TEST-DONE" "$LOG" && grep -a -q "Linux t48s" "$LOG" || { tail -n 40 "$LOG"; echo "qemu-test: FAILED" >&2; exit 1; }
grep -a "Linux t48s\|MemTotal\|timer\|uart" "$LOG" | tail -n 8
echo "qemu-test: passed"
