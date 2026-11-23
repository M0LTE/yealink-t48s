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
#   OVERLAY          directory laid over ROOTFS (default kernel6/initramfs-overlay;
#                    empty for none)
#   DEBUG=0          leave out the early debug output (kernel6/debug.config)
#   NAND=1           add the read-only flash driver (kernel6/nand.config)
#   DTS              board device tree (default bcm11188-yealink-t48s)
#   EXTRA_CONFIG     one more config fragment
#   OUT              name of the result (default zImage-t48s)
set -e
KVER=6.18.65
KSHA=c92606f956f09dc50b5a0e2af9e86763fce27ada1647de97f8d1ac8cfc95801d
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mkdir -p "${1:-.}" && cd "${1:-.}" && pwd)
DTS=${DTS:-bcm11188-yealink-t48s}
OUT=${OUT:-zImage-t48s}
CROSS=${CROSS_COMPILE:-arm-linux-gnueabi-}
[ -n "$ROOTFS" ] || { echo "set ROOTFS to Buildroot's rootfs.cpio(.gz) or firststage.cpio" >&2; exit 2; }
SRC="$WORK/linux-$KVER"
BUILD="$WORK/build-${DTS}"

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
    grep -q 'drivers/t48s/Kconfig' "$SRC/drivers/Kconfig" || sed -i 's|^endmenu$|source "drivers/t48s/Kconfig"\n\nendmenu|' "$SRC/drivers/Kconfig"
    grep -q 't48s/' "$SRC/drivers/Makefile" || echo 'obj-y += t48s/' >> "$SRC/drivers/Makefile"
fi

# the root filesystem
python3 "$HERE/mkinitramfs.py" "$WORK/initramfs-$DTS.cpio" "$ROOTFS" \
    ${OVERLAY---overlay "$HERE/initramfs-overlay"} ${OVERLAY:+--overlay "$OVERLAY"} \
    ${SYSTEM:+--file "system.img=$SYSTEM:444"} \
    $( [ -f "${AUTHORIZED_KEYS:-$WORK/root.authorized_keys}" ] && echo "--file etc/t48-dev/authorized_keys=${AUTHORIZED_KEYS:-$WORK/root.authorized_keys}:600" )

# the configuration: allnoconfig plus our fragments
mkdir -p "$BUILD"
cat "$HERE/t48s.config" > "$BUILD/all.config"
[ "${DEBUG:-1}" = 0 ] || cat "$HERE/debug.config" >> "$BUILD/all.config"
[ "${NAND:-0}" != 1 ] || cat "$HERE/nand.config" >> "$BUILD/all.config"
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
# The flash: none at all unless NAND=1, and then only drivers/t48s_nand.c, which
# cannot program or erase. Stop if the configuration holds any other NAND
# controller driver or anything that writes through MTD: block devices, UBI,
# flash filesystems, partitions from the command line, test modules.
if [ "${NAND:-0}" != 1 ]; then
    if grep -q "^CONFIG_MTD=" "$BUILD/.config"; then echo "CONFIG_MTD is set without NAND=1" >&2; exit 1; fi
else
    grep -q "PAGEPROG\|SEQIN\|ERASE1\|ERASE2\|IO_ADDR_W = [^N]" "$HERE/drivers/t48s_nand.c" && { echo "t48s_nand.c mentions a program or erase command: it must only read" >&2; exit 1; }
    for o in $(grep -o '^CONFIG_MTD_NAND_[A-Z0-9_]*=y' "$BUILD/.config" | cut -d= -f1); do
        case $o in CONFIG_MTD_NAND_CORE|CONFIG_MTD_NAND_ECC) ;; *) echo "$o is set: only t48s_nand may drive the flash" >&2; exit 1 ;; esac
    done
    for o in MTD_BLOCK MTD_BLOCK_RO MTD_CMDLINE_PARTS MTD_UBI UBIFS_FS JFFS2_FS MTD_OOPS MTD_SWAP MTD_TESTS \
             MTD_PARTITIONED_MASTER FTL NFTL INFTL RFD_FTL SSFDC SM_FTL MTD_SPI_NAND MTD_SPI_NOR MTD_CFI; do
        if grep -q "^CONFIG_$o=" "$BUILD/.config"; then echo "CONFIG_$o is set: not a way to the flash we allow" >&2; exit 1; fi
    done
    grep -q "^CONFIG_T48S_NAND=y" "$BUILD/.config"
fi

$M -s -j"$(nproc)" zImage "broadcom/$DTS.dtb"
cat "$BUILD/arch/arm/boot/zImage" "$BUILD/arch/arm/boot/dts/broadcom/$DTS.dtb" > "$WORK/$OUT"
ls -l "$BUILD/arch/arm/boot/Image" "$BUILD/arch/arm/boot/zImage" "$WORK/$OUT" | awk '{print $5, $9}'
