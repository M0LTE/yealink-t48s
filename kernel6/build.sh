#!/bin/sh
# Build the modern kernel for the T48S (issue 38). Run inside the t48s-kbuild6
# container (kernel6/Dockerfile), or on any machine with an arm-linux-gnueabi gcc:
#
#   ROOTFS=rootfs.cpio.gz sh kernel6/build.sh WORKDIR
#
# WORKDIR holds the kernel source (downloaded and checked on first use) and the
# build; this repository is only read. Result: WORKDIR/zImage-t48s, a compressed
# kernel with the device tree appended and the root filesystem built in. It is
# what tools/ourkernel.sh boot takes (kmod/t48kexec enters it like any image).
#
#   ROOTFS           Buildroot's rootfs.cpio(.gz) or firststage.cpio (required)
#   SYSTEM           Buildroot's system.img, added as /system.img (optional)
#   AUTHORIZED_KEYS  SSH keys for a development build, as kernel/build.sh (optional)
#   OVERLAY          a directory laid over ROOTFS (optional)
#   DEBUG=0          leave out the early debug output (kernel6/debug.config)
#   NAND=0           leave out the flash driver, UBI and UBIFS (kernel6/nand.config)
#   DTS              board device tree (default bcm11188-yealink-t48s)
#   EXTRA_CONFIG     one more config fragment
#   OUT              name of the result (default zImage-t48s)
#   SLOT=1           fail if the result is larger than the 2.5 MB kernel partition
#   LIVE=1           build T48S.bin for the live image instead (issue 7,
#                    docs/live-image.md): the same kernel as a U-Boot image, for
#                    recovery mode from a USB stick or TFTP. ROOTFS is the first
#                    stage; the system image is not in it (it comes in T48S.rfs,
#                    which tools/mkliveimage.py makes); keys go in only if
#                    AUTHORIZED_KEYS names a file. See "Started by U-Boot" below.
#   FLASH=1          build T48S-kernel.uimg, the file the installer writes into the
#                    phone's kernel partition (issues 8 and 29, docs/install.md):
#                    the same U-Boot image as LIVE=1, with the device tree that
#                    says t48boot=flash and names no ramdisk. ROOTFS is the first
#                    stage. It must fit the 2.5 MB partition. Keys go in only if
#                    AUTHORIZED_KEYS names a file.
#   LOADADDR         LIVE=1, FLASH=1: where U-Boot is told to copy and enter the
#                    kernel (default 0x00408000)
set -e
KVER=6.18.65
KSHA=c92606f956f09dc50b5a0e2af9e86763fce27ada1647de97f8d1ac8cfc95801d
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mkdir -p "${1:-.}" && cd "${1:-.}" && pwd)
LIVE=${LIVE:-0}
if [ "$LIVE" = 1 ]; then
    # the live kernel is the one handed to other people: no system image inside
    # (U-Boot's 4 MB would not hold it) and no keys unless a file is named
    [ -z "$SYSTEM" ] || { echo "LIVE=1: the system image goes in T48S.rfs (tools/mkliveimage.py), not in the kernel" >&2; exit 2; }
    DTS=${DTS:-bcm11188-yealink-t48s-live}
    OUT=${OUT:-T48S.bin}
    KEYS=${AUTHORIZED_KEYS:-}
    LOADADDR=${LOADADDR:-0x00408000}
fi
FLASH=${FLASH:-0}
if [ "$FLASH" = 1 ]; then
    [ "$LIVE" != 1 ] || { echo "FLASH=1 and LIVE=1 are two different files" >&2; exit 2; }
    [ -z "$SYSTEM" ] || { echo "FLASH=1: the system image goes in a flash slot, not in the kernel" >&2; exit 2; }
    DTS=${DTS:-bcm11188-yealink-t48s-flash}
    OUT=${OUT:-T48S-kernel.uimg}
    KEYS=${AUTHORIZED_KEYS:-}
    LOADADDR=${LOADADDR:-0x00408000}
fi
DTS=${DTS:-bcm11188-yealink-t48s}
OUT=${OUT:-zImage-t48s}
[ "$LIVE" = 1 ] || [ "$FLASH" = 1 ] || KEYS=${AUTHORIZED_KEYS:-$WORK/root.authorized_keys}
CROSS=${CROSS_COMPILE:-arm-linux-gnueabi-}
[ -n "$ROOTFS" ] || { echo "set ROOTFS to Buildroot's rootfs.cpio(.gz) or firststage.cpio" >&2; exit 2; }
SRC="$WORK/linux-$KVER"
# a live build differs from the ordinary one in its device tree and built-in
# files only, so it shares that build directory and costs a relink
BUILD=${DTS%-live}
BUILD="$WORK/build-${BUILD%-flash}"

# the source: the pinned release, checked, with our patches. Unpacked afresh
# whenever the patches change.
PSUM=$(cat "$HERE"/patches/*.patch | sha256sum | cut -c1-16)
if [ "$(cat "$SRC/.t48s-patches" 2>/dev/null)" != "$PSUM" ]; then
    [ -f "$WORK/linux-$KVER.tar.xz" ] || curl -fsSL -o "$WORK/linux-$KVER.tar.xz" "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz"
    echo "$KSHA  $WORK/linux-$KVER.tar.xz" | sha256sum -c - >/dev/null
    rm -rf "$SRC"
    tar -C "$WORK" -xf "$WORK/linux-$KVER.tar.xz"
    for p in "$HERE"/patches/*.patch; do
        patch -d "$SRC" -p1 -s < "$p"
    done
    echo "$PSUM" > "$SRC/.t48s-patches"
fi

# our device trees and drivers go into the tree on every build (they change often)
cp "$HERE"/dts/*.dts "$HERE"/dts/*.dtsi "$SRC/arch/arm/boot/dts/broadcom/"
if [ -f "$HERE/drivers/Makefile" ]; then
    rm -rf "$SRC/drivers/t48s"
    mkdir -p "$SRC/drivers/t48s"
    cp "$HERE"/drivers/* "$SRC/drivers/t48s/"
    # one file is shared with the 3.6 kernel as it is
    cp "$HERE"/../kernel/drivers/t48s_bootmode.c "$HERE"/../kernel/drivers/t48s_bootmode_env.h "$SRC/drivers/t48s/"
    grep -q 'drivers/t48s/Kconfig' "$SRC/drivers/Kconfig" || sed -i 's|^endmenu$|source "drivers/t48s/Kconfig"\n\nendmenu|' "$SRC/drivers/Kconfig"
    grep -q 't48s/' "$SRC/drivers/Makefile" || echo 'obj-y += t48s/' >> "$SRC/drivers/Makefile"
fi

# the root filesystem
python3 "$HERE/mkinitramfs.py" "$WORK/initramfs-$DTS.cpio" "$ROOTFS" \
    ${OVERLAY:+--overlay "$OVERLAY"} \
    ${SYSTEM:+--file "system.img=$SYSTEM:444"} \
    $( [ -n "$KEYS" ] && [ -f "$KEYS" ] && echo "--file etc/t48-dev/authorized_keys=$KEYS:600" )

# the configuration: allnoconfig plus our fragments
mkdir -p "$BUILD"
cat "$HERE/t48s.config" > "$BUILD/all.config"
[ "${DEBUG:-1}" = 0 ] || cat "$HERE/debug.config" >> "$BUILD/all.config"
[ "${NAND:-1}" != 1 ] || cat "$HERE/nand.config" >> "$BUILD/all.config"
[ -z "$EXTRA_CONFIG" ] || cat "$EXTRA_CONFIG" >> "$BUILD/all.config"
cat >> "$BUILD/all.config" <<EOC
CONFIG_INITRAMFS_SOURCE="$WORK/initramfs-$DTS.cpio"
CONFIG_INITRAMFS_COMPRESSION_NONE=y
EOC
M="make -C $SRC O=$BUILD ARCH=arm CROSS_COMPILE=$CROSS"
$M -s KCONFIG_ALLCONFIG="$BUILD/all.config" allnoconfig
# every option we asked for must have survived (a silently dropped one costs a boot)
fail=0
for o in $(grep -o '^CONFIG_[A-Z0-9_]*=[ym]' "$BUILD/all.config"); do
    grep -q "^$o\$" "$BUILD/.config" || { echo "config: $o was dropped" >&2; fail=1; }
done
[ $fail = 0 ] || exit 1
# The flash is written only through drivers/t48s_nand.c, which lets UBI reach our
# own range and nothing else (issues 29 and 51). Stop if the configuration holds
# any other NAND controller driver or any other way to the flash: block devices,
# other flash filesystems, partitions from the command line or the device tree,
# test modules. NAND=0 builds a kernel with no flash at all.
if [ "${NAND:-1}" != 1 ]; then
    if grep -q "^CONFIG_MTD=" "$BUILD/.config"; then echo "CONFIG_MTD is set with NAND=0" >&2; exit 1; fi
else
    for o in $(grep -o '^CONFIG_MTD_NAND_[A-Z0-9_]*=y' "$BUILD/.config" | cut -d= -f1); do
        case $o in CONFIG_MTD_NAND_CORE|CONFIG_MTD_NAND_ECC) ;; *) echo "$o is set: only t48s_nand may drive the flash" >&2; exit 1 ;; esac
    done
    for o in MTD_BLOCK MTD_BLOCK_RO MTD_CMDLINE_PARTS MTD_OF_PARTS MTD_REDBOOT_PARTS MTD_AFS_PARTS \
             MTD_UBI_GLUEBI MTD_UBI_BLOCK MTD_UBI_FASTMAP MTD_UBI_NVMEM MTD_UBI_FAULT_INJECTION \
             JFFS2_FS YAFFS_FS MTD_OOPS MTD_SWAP MTD_TESTS MTD_NAND_NANDSIM MTD_NAND_ECC_SW_BCH \
             MTD_PARTITIONED_MASTER FTL NFTL INFTL RFD_FTL SSFDC SM_FTL MTD_SPI_NAND MTD_SPI_NOR MTD_CFI \
             UBIFS_FS_AUTHENTICATION UBIFS_FS_ZSTD FS_ENCRYPTION; do
        if grep -q "^CONFIG_$o=" "$BUILD/.config"; then echo "CONFIG_$o is set: not a way to the flash we allow" >&2; exit 1; fi
    done
    for o in T48S_NAND MTD_UBI UBIFS_FS UBIFS_FS_LZO UBIFS_FS_ZLIB; do
        grep -q "^CONFIG_$o=y" "$BUILD/.config" || { echo "CONFIG_$o is missing" >&2; exit 1; }
    done
    grep -q '^CONFIG_MTD_UBI_BEB_LIMIT=2$' "$BUILD/.config" || { echo "CONFIG_MTD_UBI_BEB_LIMIT is not 2" >&2; exit 1; }
fi

$M -s -j"$(nproc)" zImage "broadcom/$DTS.dtb"
if [ "$LIVE" = 1 ]; then
    # Started by U-Boot. Recovery mode reads this file to 0x02680000 and T48S.rfs to
    # 0x02a80000, copies the kernel to the load address in its header and enters
    # it there with r1 = 2342 and r2 = a tag list (one memory bank and Yealink's
    # command line; no ramdisk tag, no device tree). None of that is used: the
    # kernel takes memory, command line and ramdisk from the device tree on its
    # own end (dts/t48s-live.dtsi), exactly as on a kexec start.
    #
    # The load address is 0x00408000, where the kexec loader puts the same kind of
    # file, so the unpacker runs the path already proven on the phone: it finds
    # RAM at 0x00400000 from the device tree (patches/0003), moves itself and the
    # device tree clear of where the kernel will go, and unpacks. Yealink's own
    # images (and our 3.6 one) name 0x0e000000, which this kernel must not: the
    # device tree would stay there, outside the RAM the kernel is told about, and
    # nothing would keep its memory from being handed out.
    # tools/mkliveimage.py checkbin proves the addresses from the finished file.
    cat "$BUILD/arch/arm/boot/zImage" "$BUILD/arch/arm/boot/dts/broadcom/$DTS.dtb" > "$WORK/$OUT.zimage"
    mkimage -A arm -O linux -T kernel -C none -a "$LOADADDR" -e "$LOADADDR" -n "Linux-$KVER-t48s" -d "$WORK/$OUT.zimage" "$WORK/$OUT" >/dev/null
    rm -f "$WORK/$OUT.zimage"
    ls -l "$BUILD/arch/arm/boot/Image" "$BUILD/arch/arm/boot/zImage" "$WORK/$OUT" | awk '{print $5, $9}'
    python3 "$HERE/../tools/mkliveimage.py" checkbin "$WORK/$OUT" kernel=6.18
    exit 0
fi
if [ "$FLASH" = 1 ]; then
    # Started by U-Boot from flash: "nboot 0x02680000 0 0x015A0000" reads the size
    # from this file's header, then bootm checks both CRCs, copies the kernel to
    # the load address and enters it, exactly as for LIVE=1 above. So it is the
    # same kind of file with another device tree, and it must fit the partition.
    # tools/t48flash.py checkkernel proves the header, the addresses and the size
    # from the finished file.
    cat "$BUILD/arch/arm/boot/zImage" "$BUILD/arch/arm/boot/dts/broadcom/$DTS.dtb" > "$WORK/$OUT.zimage"
    mkimage -A arm -O linux -T kernel -C none -a "$LOADADDR" -e "$LOADADDR" -n "Linux-$KVER-t48s" -d "$WORK/$OUT.zimage" "$WORK/$OUT" >/dev/null
    rm -f "$WORK/$OUT.zimage"
    ls -l "$BUILD/arch/arm/boot/Image" "$BUILD/arch/arm/boot/zImage" "$WORK/$OUT" | awk '{print $5, $9}'
    python3 "$HERE/../tools/t48flash.py" checkkernel "$WORK/$OUT"
    exit 0
fi
cat "$BUILD/arch/arm/boot/zImage" "$BUILD/arch/arm/boot/dts/broadcom/$DTS.dtb" > "$WORK/$OUT"
ls -l "$BUILD/arch/arm/boot/Image" "$BUILD/arch/arm/boot/zImage" "$WORK/$OUT" | awk '{print $5, $9}'
# SLOT=1: the result must fit the kernel partition (2.5 MB). Meant for a build with
# the first stage alone, which is what goes there (docs/boot.md).
if [ "${SLOT:-0}" = 1 ]; then
    size=$(wc -c < "$WORK/$OUT")
    echo "kernel slot: $size of 2621440 bytes used, $((2621440 - size)) free"
    [ "$size" -le 2621440 ] || { echo "$OUT does not fit the 2.5 MB kernel slot" >&2; exit 1; }
fi
