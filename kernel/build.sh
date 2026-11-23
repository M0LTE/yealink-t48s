#!/bin/sh
# Build the T48S kernel inside the t48s-kbuild:trusty container (gcc 4.7).
# Run from the directory holding linux-3.6.11/ and this kernel/ directory.
#   sh kernel/build.sh          -> T48S.bin   (U-Boot image, for recovery-mode TFTP boot)
#   ROOTFS=rootfs.cpio sh kernel/build.sh kexec
#                               -> Image-kexec (raw Image for kmod/t48kexec) with
#                                  Buildroot's root filesystem (buildroot/build.sh)
#                                  built in as the initramfs. If AUTHORIZED_KEYS
#                                  (default kexec-rootfs/root.authorized_keys)
#                                  exists, it is added as /root/.ssh/authorized_keys.
set -e
VARIANT=${1:-tftp}
if [ "$VARIANT" = kexec ]; then
    [ -n "$ROOTFS" ] || { echo "kexec: set ROOTFS to Buildroot's rootfs.cpio (buildroot/build.sh)" >&2; exit 2; }
    # the initramfs is Buildroot's cpio with a second cpio holding the keys appended
    # (the kernel unpacks concatenated archives in turn); the keys never enter the
    # Buildroot output, so a published root filesystem carries none
    python3 - "$ROOTFS" kexec-initramfs.cpio "${AUTHORIZED_KEYS:-kexec-rootfs/root.authorized_keys}" <<'EOP'
import os, sys
src, dst, keys = sys.argv[1:4]
out = bytearray(open(src, "rb").read())
def entry(ino, name, mode, nlink, body=b""):
    name = name.encode() + b"\0"
    hdr = "070701" + "".join("%08X" % v for v in
        (ino, mode, 0, 0, nlink, 0, len(body), 0, 0, 0, 0, len(name), 0))
    rec = hdr.encode() + name
    rec += b"\0" * (-len(rec) % 4)
    return rec + body + b"\0" * (-len(body) % 4)
if os.path.exists(keys):
    out += b"\0" * (-len(out) % 4)
    out += entry(1, "root/.ssh", 0o40700, 2)
    out += entry(2, "root/.ssh/authorized_keys", 0o100600, 1, open(keys, "rb").read())
    out += entry(0, "TRAILER!!!", 0, 1)
    print("initramfs: added %s as /root/.ssh/authorized_keys" % keys)
else:
    print("initramfs: no %s, so no SSH keys (serial console only)" % keys)
open(dst, "wb").write(out)
EOP
    echo "CONFIG_INITRAMFS_SOURCE=\"$(pwd)/kexec-initramfs.cpio\"" > kexec-initramfs.config
fi
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
# DMA: controller 0 carries audio. Yealink's firmware runs four of its channels for the
# APM codecs, and writing an active channel's registers faults. The kexec loader stops
# them all, but the DMA framework still must not reset or claim anything there: the
# only user is drivers/t48s_audio.c, which programs its own channels (3 and 5) directly
# and touches no other. So leave controller 0 alone here, and stop each controller 1
# channel before resetting its configuration.
python3 - <<'EOP'
p = "arch/arm/mach-bcmring/csp/dmac/dmacHw.c"
s = open(p).read()
old = "\tpCblk->channel = channel;\n\n\t/* Enable DMA controller */"
new = ("\tpCblk->channel = channel;\n\n"
       "\t/* T48S: controller 0 is left to t48s_audio; do not touch it */\n"
       "\tif (module == 0)\n\t\treturn 0;\n"
       "\t/* T48S: stop the channel before resetting its configuration */\n"
       "\t{\n\t\tint n = 100000;\n"
       "\t\tdmacHw_DMA_STOP(module, channel);\n"
       "\t\twhile ((dmacHw_REG_MISC_CH_ENABLE(module) & (1 << channel)) && --n)\n\t\t\t;\n\t}\n\n"
       "\t/* Enable DMA controller */")
assert s.count(old) == 1
open(p, "w").write(s.replace(old, new))
EOP
grep -q "controller 0 is left to t48s_audio" arch/arm/mach-bcmring/csp/dmac/dmacHw.c
# ... and register no interrupt handlers for controller 0's channels: t48s_audio
# requests the ones it uses, and the rest stay masked
python3 - <<'EOP'
p = "arch/arm/mach-bcmring/dma.c"
s = open(p).read()
old = "\t\t\tif ((channel->flags & DMA_CHANNEL_FLAG_NO_ISR) == 0) {\n\t\t\t\tsnprintf(channel->name"
new = ("\t\t\tif (controllerIdx != 0 && /* T48S: controller 0 is t48s_audio's */\n"
       "\t\t\t    (channel->flags & DMA_CHANNEL_FLAG_NO_ISR) == 0) {\n\t\t\t\tsnprintf(channel->name")
assert s.count(old) == 1, s.count(old)
open(p, "w").write(s.replace(old, new))
EOP
grep -q "controller 0 is t48s_audio's" arch/arm/mach-bcmring/dma.c
# the ESW channels (DMA1 2 and 3) belong to drivers/t48s_eth.c, which programs them
# directly: keep the DMA framework from requesting their interrupts
python3 - <<'EOP'
p = "arch/arm/mach-bcmring/dma_device.c"
s = open(p).read()
for ch in ("3", "2"):
    old = "\t .flags = DMA_DEVICE_FLAG_IS_DEDICATED,\n\t .dedicatedController = 1,\n\t .dedicatedChannel = %s," % ch
    new = "\t .flags = DMA_DEVICE_FLAG_IS_DEDICATED | DMA_DEVICE_FLAG_NO_ISR, /* T48S: t48s_eth */\n\t .dedicatedController = 1,\n\t .dedicatedChannel = %s," % ch
    assert s.count(old) == 1, ch
    s = s.replace(old, new)
open(p, "w").write(s)
EOP
grep -c "T48S: t48s_eth" arch/arm/mach-bcmring/dma_device.c | grep -q 2
# our ethernet driver
cp ../kernel/drivers/t48s_eth.c drivers/net/ethernet/t48s_eth.c
grep -q "t48s_eth.o" drivers/net/ethernet/Makefile || echo "obj-y += t48s_eth.o" >> drivers/net/ethernet/Makefile
# our screen driver, with a Kconfig entry so the cfb drawing helpers get selected
cp ../kernel/drivers/t48s_fb.c drivers/video/t48s_fb.c
grep -q "FB_T48S" drivers/video/Kconfig || sed -i 's/^config FB_ARMCLCD$/config FB_T48S\n\tbool "Yealink T48S screen (PL111 left running by U-Boot)"\n\tdepends on FB = y\n\tselect FB_CFB_FILLRECT\n\tselect FB_CFB_COPYAREA\n\tselect FB_CFB_IMAGEBLIT\n\nconfig FB_ARMCLCD/' drivers/video/Kconfig
grep -q "FB_T48S" drivers/video/Kconfig
grep -q "t48s_fb.o" drivers/video/Makefile || echo 'obj-$(CONFIG_FB_T48S) += t48s_fb.o' >> drivers/video/Makefile
# our keypad and hook switch driver, and the Holtek chip (its watchdog)
cp ../kernel/drivers/t48s_keypad.c drivers/input/t48s_keypad.c
grep -q "t48s_keypad.o" drivers/input/Makefile || echo 'obj-$(CONFIG_INPUT) += t48s_keypad.o' >> drivers/input/Makefile
cp ../kernel/drivers/t48s_holtek.c drivers/misc/t48s_holtek.c
grep -q "t48s_holtek.o" drivers/misc/Makefile || echo "obj-y += t48s_holtek.o" >> drivers/misc/Makefile
# the lamps (LED class; the power lamp goes through the Holtek driver)
cp ../kernel/drivers/t48s_holtek.h include/linux/t48s_holtek.h
cp ../kernel/drivers/t48s_leds.c drivers/leds/t48s_leds.c
grep -q "t48s_leds.o" drivers/leds/Makefile || echo 'obj-$(CONFIG_LEDS_CLASS) += t48s_leds.o' >> drivers/leds/Makefile
# our touchscreen driver (TSC2046 on the PL022 SPI host, polled)
cp ../kernel/drivers/t48s_touch.c drivers/input/t48s_touch.c
grep -q "t48s_touch.o" drivers/input/Makefile || echo 'obj-$(CONFIG_INPUT) += t48s_touch.o' >> drivers/input/Makefile
# our microphone capture driver (ALSA; the APM codecs over DMA0 channels 3 and 5), with
# a Kconfig entry so the ALSA PCM core gets selected
cp ../kernel/drivers/t48s_audio.c sound/arm/t48s_audio.c
grep -q "SND_T48S" sound/arm/Kconfig || printf '\nconfig SND_T48S\n\tbool "Yealink T48S audio (BCM11188 APM codecs)"\n\tdepends on SND = y\n\tselect SND_PCM\n' >> sound/arm/Kconfig
grep -q "t48s_audio.o" sound/arm/Makefile || echo 'obj-$(CONFIG_SND_T48S) += t48s_audio.o' >> sound/arm/Makefile
# put the delay loop on an 8-byte boundary. On this ARM1176 the two-instruction loop
# takes 2 cycles per pass there and 3 at the other word alignment, and where it lands is
# an accident of the link: unaligned, 3.6 measured 331.77 BogoMIPS against the 498.07
# of Yealink's kernel (whose loop happens to sit at 0xc014bab0) on the same 500 MHz core
sed -i 's/^ENTRY(__loop_delay)$/\t\t.align\t3\t\t\t@ T48S: see kernel\/build.sh\nENTRY(__loop_delay)/' arch/arm/lib/delay-loop.S
grep -q "T48S: see kernel/build.sh" arch/arm/lib/delay-loop.S
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
[ "$VARIANT" = kexec ] && ./scripts/kconfig/merge_config.sh -m .config ../kernel/kexec.config >/dev/null
[ "$VARIANT" = kexec ] && ./scripts/kconfig/merge_config.sh -m .config ../kexec-initramfs.config >/dev/null
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- olddefconfig >/dev/null 2>&1 || make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- oldnoconfig >/dev/null
for o in CONFIG_LEDS_CLASS= CONFIG_LEDS_TRIGGER_TIMER= CONFIG_SND_T48S CONFIG_SND_PCM= CONFIG_DEBUG_LL= CONFIG_EARLY_PRINTK CONFIG_PHYS_OFFSET CONFIG_BCM_ZRELADDR CONFIG_CMDLINE= CONFIG_CMDLINE_FORCE CONFIG_MTD CONFIG_ARM_PATCH_PHYS_VIRT CONFIG_BLK_DEV_RAM= CONFIG_DEVTMPFS=; do grep "^$o\|# $o" .config || echo "MISSING $o"; done
if [ "$VARIANT" = kexec ]; then
    make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j12 Image 2>&1 | tail -n 3
    cp arch/arm/boot/Image ../Image-kexec
    exit 0
fi
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j12 zImage 2>&1 | tail -n 5
mkimage -A arm -O linux -T kernel -C none -a 0x0e000000 -e 0x0e000000 -n "Linux-3.6.11-t48s" -d arch/arm/boot/zImage ../T48S.bin
