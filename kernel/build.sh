#!/bin/sh
# Build the T48S kernel inside the t48s-kbuild:trusty container (gcc 4.7).
# Run from the directory holding linux-3.6.11/ and this kernel/ directory.
set -e
cd linux-3.6.11
# mainline gives bcmring machine number 2289; the phone's U-Boot passes 2342
sed -i 's/^\(bcmring[[:space:]]*MACH_BCMRING[[:space:]]*BCMRING[[:space:]]*\)2289$/\12342/' arch/arm/tools/mach-types
grep -q "BCMRING[[:space:]]*2342" arch/arm/tools/mach-types
# RAM from 0x00400000, stopping below the framebuffer U-Boot set up at 0x07A24000
sed -i 's/t->u.mem.start = CFG_GLOBAL_RAM_BASE;/t->u.mem.start = 0x00400000; \/* T48S *\//; s/t->u.mem.size = CFG_GLOBAL_RAM_SIZE;/t->u.mem.size = 0x07620000; \/* T48S: up to the framebuffer *\//' arch/arm/mach-bcmring/arch.c
grep -q "T48S: up to the framebuffer" arch/arm/mach-bcmring/arch.c
# leave the PLLs exactly as U-Boot set them
sed -i 's/^\tchipcHw_enableSpreadSpectrum();/\t\/* T48S: chipcHw_enableSpreadSpectrum() skipped *\//' arch/arm/mach-bcmring/arch.c
grep -q "spreadSpectrum() skipped\|SpreadSpectrum() skipped" arch/arm/mach-bcmring/arch.c
# the T48S UART clock measured at 100 MHz (mainline assumes 142.857 MHz)
sed -i 's/#define HW_CFG_UART_CLK_HZ           142857142/#define HW_CFG_UART_CLK_HZ           100000000 \/* T48S, measured *\//' arch/arm/mach-bcmring/include/mach/csp/hw_cfg.h
grep -q "100000000 /\* T48S" arch/arm/mach-bcmring/include/mach/csp/hw_cfg.h
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- bcmring_defconfig
./scripts/kconfig/merge_config.sh -m .config ../kernel/t48s.config >/dev/null
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- olddefconfig >/dev/null 2>&1 || make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- oldnoconfig >/dev/null
for o in CONFIG_PHYS_OFFSET CONFIG_BCM_ZRELADDR CONFIG_CMDLINE= CONFIG_CMDLINE_FORCE CONFIG_MTD CONFIG_ARM_PATCH_PHYS_VIRT CONFIG_BLK_DEV_RAM= CONFIG_DEVTMPFS=; do grep "^$o\|# $o" .config || echo "MISSING $o"; done
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j12 zImage 2>&1 | tail -n 5
mkimage -A arm -O linux -T kernel -C none -a 0x0e000000 -e 0x0e000000 -n "Linux-3.6.11-t48s" -d arch/arm/boot/zImage ../T48S.bin
