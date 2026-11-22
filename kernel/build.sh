#!/bin/sh
# Build the T48S kernel inside the t48s-kbuild:trusty container (gcc 4.7).
# Run from the directory holding linux-3.6.11/ and this kernel/ directory.
set -e
cd linux-3.6.11
# mainline gives bcmring machine number 2289; the phone's U-Boot passes 2342
sed -i 's/^\(bcmring[[:space:]]*MACH_BCMRING[[:space:]]*BCMRING[[:space:]]*\)2289$/\12342/' arch/arm/tools/mach-types
grep -q "BCMRING[[:space:]]*2342" arch/arm/tools/mach-types
# RAM from 0x00400000 to 0x07A00000: below the framebuffer U-Boot set up at 0x07A24000,
# and ending on a 1 MB boundary (3.6 hangs in paging_init if lowmem ends mid-section)
sed -i 's/t->u.mem.start = CFG_GLOBAL_RAM_BASE;/t->u.mem.start = 0x00400000; \/* T48S *\//; s/t->u.mem.size = CFG_GLOBAL_RAM_SIZE;/t->u.mem.size = 0x07600000; \/* T48S: to 0x07A00000, 1 MB aligned, below the framebuffer *\//' arch/arm/mach-bcmring/arch.c
grep -q "T48S: to 0x07A00000" arch/arm/mach-bcmring/arch.c
# drop the UMI and NAND static mappings: they overlap each other and sit inside the
# 16 MB CHIPC mapping (0x80000000 -> 0xF8000000), and 3.6 BUG()s on overlapping io maps
sed -i '/IO_DESC(MM_IO_BASE_NAND, SZ_64K)/d; /IO_DESC(MM_IO_BASE_UMI, SZ_64K)/d' arch/arm/mach-bcmring/mm.c
! grep -q "MM_IO_BASE_NAND\|MM_IO_BASE_UMI" arch/arm/mach-bcmring/mm.c
# the UART clocks are registered for "uarta"/"uartb" but the AMBA devices are named
# "uartA"/"uartB", so the PL011 driver finds no clock (probe error -2) and the
# console dies when the boot console hands over
sed -i 's/\.dev_id = "uarta",/.dev_id = "uartA",/; s/\.dev_id = "uartb",/.dev_id = "uartB",/' arch/arm/mach-bcmring/core.c
grep -q '.dev_id = "uartA"' arch/arm/mach-bcmring/core.c
# the UART clock is already running (U-Boot set it up); start its use count at 1 so
# the AMBA bus's enable/disable around reading the peripheral ID does not gate it off
# under the boot console
sed -i 's/^\t.rate_hz = HW_CFG_UART_CLK_HZ,$/\t.rate_hz = HW_CFG_UART_CLK_HZ,\n\t.use_cnt = 1, \/* T48S: on from U-Boot *\//' arch/arm/mach-bcmring/core.c
grep -q "use_cnt = 1, /\* T48S" arch/arm/mach-bcmring/core.c
# leave the PLLs exactly as U-Boot set them
sed -i 's/^\tchipcHw_enableSpreadSpectrum();/\t\/* T48S: chipcHw_enableSpreadSpectrum() skipped *\//' arch/arm/mach-bcmring/arch.c
grep -q "spreadSpectrum() skipped\|SpreadSpectrum() skipped" arch/arm/mach-bcmring/arch.c
# the T48S UART clock measured at 100 MHz (mainline assumes 142.857 MHz)
sed -i 's/#define HW_CFG_UART_CLK_HZ           142857142/#define HW_CFG_UART_CLK_HZ           100000000 \/* T48S, measured *\//' arch/arm/mach-bcmring/include/mach/csp/hw_cfg.h
grep -q "100000000 /\* T48S" arch/arm/mach-bcmring/include/mach/csp/hw_cfg.h
# low-level debug output for this platform (mainline has none): PL011 UARTA,
# physical 0x80012000, virtual 0xF8012000 (CHIPC block mapped at 0xF8000000 by mm.c)
cat > arch/arm/mach-bcmring/include/mach/debug-macro.S <<'EOD'
/* T48S: early debug output on UARTA (PL011) */
		.macro	addruart, rp, rv, tmp
		ldr	\rp, =0x80012000
		ldr	\rv, =0xf8012000
		.endm
#include <asm/hardware/debug-pl01x.S>
EOD
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- bcmring_defconfig
./scripts/kconfig/merge_config.sh -m .config ../kernel/t48s.config >/dev/null
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- olddefconfig >/dev/null 2>&1 || make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- oldnoconfig >/dev/null
for o in CONFIG_DEBUG_LL= CONFIG_EARLY_PRINTK CONFIG_PHYS_OFFSET CONFIG_BCM_ZRELADDR CONFIG_CMDLINE= CONFIG_CMDLINE_FORCE CONFIG_MTD CONFIG_ARM_PATCH_PHYS_VIRT CONFIG_BLK_DEV_RAM= CONFIG_DEVTMPFS=; do grep "^$o\|# $o" .config || echo "MISSING $o"; done
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j12 zImage 2>&1 | tail -n 5
mkimage -A arm -O linux -T kernel -C none -a 0x0e000000 -e 0x0e000000 -n "Linux-3.6.11-t48s" -d arch/arm/boot/zImage ../T48S.bin
