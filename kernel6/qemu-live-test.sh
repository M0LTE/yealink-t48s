#!/bin/sh
# The live start (LIVE=1, docs/live-image.md) on a PC, as far as a PC can go:
# build T48S.bin for QEMU's versatilepb machine (dts/qemu-versatilepb-test-live.dts:
# the test machine with the live start's ramdisk and command line), then hand it
# over the way the phone's U-Boot does:
#   - T48S.bin read to 0x02680000 and T48S.rfs (tools/mkliveimage.py) to 0x02a80000;
#   - the kernel copied to the load address in its header and entered there with
#     r0 = 0, r1 = 2342, r2 = a tag list at 0x00400100 holding what U-Boot's holds:
#     one memory bank and Yealink's command line, no ramdisk tag.
# It passes if the kernel placed itself at 0x00400000, took the ramdisk from the
# device tree and unpacked /system.img out of it, used the device tree's command
# line and none of U-Boot's, and the first stage then started that system image
# and it reads back byte for byte (its SHA-256, from the running system). It proves nothing about the phone's own hardware or its U-Boot.
#   ROOTFS=firststage.cpio SYSTEM=system.img sh kernel6/qemu-live-test.sh WORKDIR
# (needs qemu-system-arm). BAD=1 hands over a ramdisk with rubbish after the
# archive instead (a T48S.rfs that was not padded): the kernel must say so and
# still come up. Unlike 3.6, this kernel keeps what it unpacked before the rubbish.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mkdir -p "${1:-.}" && cd "${1:-.}" && pwd)
[ -n "$SYSTEM" ] || { echo "set SYSTEM to Buildroot's system.img (and ROOTFS to firststage.cpio)" >&2; exit 2; }
SYS=$SYSTEM
SYSTEM= LIVE=1 DTS=qemu-versatilepb-test-live DEBUG=0 NAND=0 OUT=T48S-qemu.bin sh "$HERE/build.sh" "$WORK"
python3 "$HERE/../tools/mkliveimage.py" rfs "$SYS" "$WORK/T48S-qemu.rfs"
python3 - "$WORK" "${BAD:-0}" <<'EOP'
import struct, sys
work, bad = sys.argv[1], sys.argv[2] == "1"
b = open(work + "/T48S-qemu.bin", "rb").read()
size, load, entry = struct.unpack_from(">3I", b, 12)
open(work + "/live-payload.bin", "wb").write(b[64:64 + size])
# U-Boot's tag list (boot_jump_linux at 0x03B20448): CORE, MEM, CMDLINE, NONE
cmd = (b"eth=80:5e:c0:00:00:01 initrd=0x02a80000,0x800000 root=/dev/ram0 rw rootfstype=ext2 "
       b"vmalloc=0x08000000 mem=124M hwversion=1 fb=0x07A24000:0x005DC000 ATAGMARKER=1\0")
cmd += b"\0" * (-len(cmd) % 4)
tags = struct.pack("<5I", 5, 0x54410001, 0, 0, 0)
tags += struct.pack("<4I", 4, 0x54410002, 0x07c00000, 0x00400000)
tags += struct.pack("<2I", 2 + len(cmd) // 4, 0x54410009) + cmd
tags += struct.pack("<2I", 0, 0)
open(work + "/live-atags.bin", "wb").write(tags)
# the jump: mov r0, #0; ldr r1, machine; ldr r2, tags; ldr pc, entry
stub = struct.pack("<7I", 0xe3a00000, 0xe59f1004, 0xe59f2004, 0xe59ff004, 2342, 0x00400100, entry)
open(work + "/live-stub.bin", "wb").write(stub)
if bad:
    r = bytearray(open(work + "/T48S-qemu.rfs", "rb").read())
    r[-4096:] = bytes((i * 7 + 1) & 0xff or 1 for i in range(4096))
    open(work + "/T48S-qemu.rfs", "wb").write(r)
open(work + "/live-load", "w").write("0x%08x" % load)
EOP
LOAD=$(cat "$WORK/live-load")
LOG="$WORK/qemu-live-test.log"
ld() { echo "-device loader,file=$WORK/$1,addr=$2,force-raw=on"; }
# U-Boot's copy to the load address (none if that is where the file already lies)
PAY=$([ "$LOAD" = 0x02680040 ] || ld live-payload.bin "$LOAD")
( sleep 40; echo; echo 'cat /proc/cmdline; sha256sum /dev/loop0 /system.img 2>&1; grep " / " /proc/mounts; dmesg | grep -i "initrd\|initramfs\|Memory:"; echo QEMU-LIVE-DONE'; sleep 8 ) |
    timeout 90 qemu-system-arm -M versatilepb -cpu arm1176 -m 128 -nographic -no-reboot \
        $(ld T48S-qemu.bin 0x02680000) $(ld T48S-qemu.rfs 0x02a80000) $PAY \
        $(ld live-atags.bin 0x00400100) $(ld live-stub.bin 0x03b00000) \
        -device loader,addr=0x03b00000,cpu-num=0 > "$LOG" 2>&1 || true
fail() { tail -n 60 "$LOG"; echo "qemu-live-test: FAILED: $1" >&2; exit 1; }
grep -a -q "QEMU-LIVE-DONE" "$LOG" || fail "no shell"
grep -a -q "t48boot=live" "$LOG" || fail "the device tree's command line was not used"
! grep -a -q "ATAGMARKER" "$LOG" || fail "U-Boot's command line got in"
if [ "${BAD:-0}" = 1 ]; then
    grep -a -q "Initramfs unpacking failed\|is not initramfs" "$LOG" || fail "the damaged ramdisk was not reported"
    echo "qemu-live-test: passed (rubbish after the archive reported; the system still came up)"
    exit 0
fi
# the first stage has made the image the root by now: it is the loop device
grep -a -q "starting the system image /system.img" "$LOG" || fail "the first stage did not start the system image"
grep -a -q "$(sha256sum "$SYS" | cut -d' ' -f1)  /dev/loop0" "$LOG" || fail "the running system image is not the one that went in"
grep -a "Memory:\|initrd\|system" "$LOG" | tail -n 12
echo "qemu-live-test: passed"
