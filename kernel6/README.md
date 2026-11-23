# kernel6: a current kernel for the T48S

Issue [#38](https://github.com/M0LTE/yealink-t48s/issues/38). A feasibility spike: can the phone leave Linux 3.6.11 (2012) for a kernel that is still maintained? **`kernel/` (3.6.11) is still the phone's kernel.** This one lives beside it until it can do everything the old one does.

## Which kernel, which compiler (issue #39)

**Linux 6.18 LTS** (6.18.65 pinned in `build.sh`), built with Debian 13's stock `arm-linux-gnueabi` gcc 14.

- 6.18 is the newest long-term kernel (November 2025). kernel.org lists it and 6.12 with the same end of life, December 2028, so the older one buys nothing.
- It still supports this CPU. ARMv6 is the oldest ARM generation the kernel keeps, and the ARM1176 in particular is the one that stays in the newest development kernels, because the first Raspberry Pi and the Pi Zero use it. So there is a path beyond 6.18 too.
- No special compiler: the kernel's own flags select ARMv6 and soft-float. The 2012 gcc 4.7 container that 3.6 needs is not involved.

## How it differs from `kernel/`

3.6.11 had Broadcom's platform code for this chip family (`mach-bcmring`), which mainline deleted in 3.7, before device trees reached it. Nothing in a modern kernel knows the chip. So here:

- **The hardware is described in a device tree** that we wrote: `dts/bcm11188.dtsi` (the SoC) and `dts/bcm11188-yealink-t48s.dts` (the phone).
- **There is no platform code.** One small patch (`patches/`) adds a Kconfig entry that switches on the standard drivers the SoC needs. The parts are ordinary ARM ones and the kernel's own drivers run them: PL192 interrupt controllers, SP804 timers, PL011 serial ports.
- **No clock driver.** U-Boot sets up the PLLs; the device tree states the rates it leaves.
- **The configuration starts from nothing** (`allnoconfig` plus `t48s.config`), because the kernel has to fit a 2.5 MB flash slot.
- **No flash driver yet**, on purpose: `build.sh` refuses to build a configuration that has one, until the NAND driver is ported and checked (issue #51).

## Building

```
docker build -t t48s-kbuild6:trixie kernel6
docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w -e ROOTFS=/w/rootfs.cpio.gz t48s-kbuild6:trixie sh kernel6/build.sh /w/work
```

`ROOTFS` is Buildroot's root filesystem (`buildroot/`), as for `kernel/build.sh`. The result is `work/zImage-t48s`: the compressed kernel with the device tree on the end and the root filesystem inside. A rebuild after changing a driver or the device tree takes well under a minute, because the source tree is kept.

For now `initramfs-overlay/` replaces the root filesystem's start-up script with a short one that brings up only what this kernel has drivers for.

## Booting

The same way as the old kernel, with the same loader and no changes to it:

```
sh tools/ourkernel.sh boot work/zImage-t48s
```

The loader copies the file to 0x00408000 and jumps to it. This file is a self-extracting kernel: it finds the device tree on its own end, reads where RAM starts from it (0x00400000), unpacks itself there and starts. The loader passes no device tree and needs to know nothing about one.

## Testing without the phone

`qemu-test.sh` builds the same configuration for QEMU's `versatilepb` machine with an ARM1176 and boots it to the shell. That machine has the same kinds of parts at other addresses, so it checks the configuration, the boot without platform code, and that the root filesystem runs on this kernel. It says nothing about the phone's own hardware.

## Status

See the checklist in issue #38.
