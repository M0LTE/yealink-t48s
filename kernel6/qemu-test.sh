#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Smoke test on a PC: build the T48S kernel configuration for QEMU's versatilepb
# machine (ARM1176, see dts/qemu-versatilepb-test.dts) and boot it to the shell
# prompt. It proves the configuration, the device-tree-only boot, the interrupt
# controller, timer and serial drivers, and that the root filesystem runs on this
# kernel, and that the key lamps' driver dims three pins from its timer. The
# image is put where the phone's loader puts it (0x00408000, nothing in the
# registers) and the first 4 MB are not declared as RAM, as on the phone,
# so the kernel has to place itself at 0x00400000 from the appended device tree
# (patches/0003). It proves nothing about the phone's own hardware.
#   ROOTFS=rootfs.cpio.gz sh kernel6/qemu-test.sh WORKDIR     (needs qemu-system-arm)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-.}
echo CONFIG_GPIO_PL061=y > "$WORK/qemu.config"
DTS=qemu-versatilepb-test DEBUG=0 OUT=zImage-qemu EXTRA_CONFIG="$WORK/qemu.config" sh "$HERE/build.sh" "$WORK"
LOG="$WORK/qemu-test.log"
# the lamps: one dim, one half, one nearly full for a second, then all dark; about 200 frames and a stopped timer.
# The shell is stopped first: it has no screen here, starts over and over, and puts the lamps out each time.
LAMPS='sv d /run/service/shell; cd /sys/class/leds; P=/sys/module/t48s_lamps/parameters; echo 1 > t48s:green:headset/brightness; echo 128 > t48s:red:mute/brightness; echo 254 > t48s:green:speaker/brightness; sleep 1; for l in t48s:*; do echo 0 > $l/brightness; done; sleep 1; A=$(cat $P/frames); sleep 1; B=$(cat $P/frames); echo "lamps: $A frames, then $((B - A)), late $(cat $P/late_us) $(cat $P/off_late_us) us"; [ $A -gt 150 ] && [ $A -lt 260 ] && [ $B = $A ] && echo LAMPS-OK'
( sleep 25; echo "$LAMPS"; sleep 4; echo 'uname -a; cat /proc/interrupts; grep MemTotal /proc/meminfo; sleep 2; date; echo QEMU-TEST-DONE'; sleep 8 ) |
    timeout 70 qemu-system-arm -M versatilepb -cpu arm1176 -m 128 -nographic -no-reboot \
        -device loader,file="$WORK/zImage-qemu",addr=0x00408000,cpu-num=0,force-raw=on > "$LOG" 2>&1 || true
grep -a -q "QEMU-TEST-DONE" "$LOG" && grep -a -q "Linux t48s" "$LOG" && grep -a -q "^LAMPS-OK" "$LOG" || { tail -n 40 "$LOG"; echo "qemu-test: FAILED" >&2; exit 1; }
grep -a "Linux t48s\|MemTotal\|timer\|uart\|^lamps:" "$LOG" | tail -n 8
echo "qemu-test: passed"
