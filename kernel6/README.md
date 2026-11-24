# kernel6: the phone's kernel

Issues [#38](https://github.com/M0LTE/yealink-t48s/issues/38) and [#55](https://github.com/M0LTE/yealink-t48s/issues/55). **Linux 6.18 is the phone's kernel since 2026-11-24**: it is what CI builds as `Image-kexec` and the live set, and what the release carries. It began as a spike to leave Linux 3.6.11 (2012) for a kernel that is still maintained. [`kernel/`](../kernel/README.md) (3.6.11) is kept as a fallback build, `Image-kexec-linux3.6`, for one release.

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
- **The flash driver writes only our `/data` range**, like the one in `kernel/`: see [Flash](#flash) below. `build.sh` stops if the configuration holds any other way to the flash.

## Building

```
docker build -t t48s-kbuild6:trixie kernel6
docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w -e ROOTFS=/w/rootfs.cpio.gz t48s-kbuild6:trixie sh kernel6/build.sh /w/work
```

`ROOTFS` is Buildroot's first stage or root filesystem (`buildroot/`), as for `kernel/build.sh`; the same one serves both kernels. The result is `work/zImage-t48s`: the compressed kernel with the device tree on the end and the root filesystem inside. A rebuild after changing a driver or the device tree takes well under a minute, because the source tree is kept.

```
ROOTFS=firststage.cpio SYSTEM=system.img OUT=Image-kexec    # the whole system, for kexec (what CI publishes)
ROOTFS=firststage.cpio SLOT=1                               # first stage alone; fails if over the 2.5 MB kernel slot
ROOTFS=firststage.cpio LIVE=1                               # T48S.bin for U-Boot's recovery mode (stick or TFTP)
```

`LIVE=1` makes the kernel file of the live image ([docs/live-image.md](../docs/live-image.md), "The same with Linux 6.18"): a U-Boot image of the same kernel with `dts/bcm11188-yealink-t48s-live.dts`, which adds the ramdisk U-Boot loads and `t48boot=live` to the device tree. U-Boot's own hand-over (tags, no device tree) is not used. The build ends by checking the file's addresses (`tools/mkliveimage.py checkbin`).

CI (`.github/workflows/build.yml`) builds all three and publishes `Image-kexec` and the live set (`t48s-live.tar.gz`). The 3.6 fallback, `Image-kexec-linux3.6`, is built from the same first stage and system image.

A development build (SSH on from boot, with our keys) takes `AUTHORIZED_KEYS=FILE`, or a file `root.authorized_keys` in the work directory. Keys are never in this repository ([docs/access.md](../docs/access.md#our-own-test-builds)).

## Booting

By kexec from Yealink's firmware, with the loader in `kmod/`:

```
sh tools/ourkernel.sh boot work/Image-kexec
```

The loader copies the file to 0x00408000 and jumps to it. This file is a self-extracting kernel: it finds the device tree on its own end, reads where RAM starts from it (0x00400000), unpacks itself there and starts. The loader passes no device tree and needs to know nothing about one.

## Testing without the phone

`qemu-test.sh` builds the same configuration for QEMU's `versatilepb` machine with an ARM1176 and boots it to the shell. That machine has the same kinds of parts at other addresses, so it checks the configuration, the boot without platform code, and that the root filesystem runs on this kernel. It says nothing about the phone's own hardware.

`qemu-live-test.sh` does the same for a live start: it lays `T48S.bin` and `T48S.rfs` out in memory as the phone's U-Boot does, enters the kernel with U-Boot's registers and a copy of its tag list, and passes if the system image from the ramdisk ends up running, unchanged, with our command line.

## Status (2026-11-24)

**Tom's hands-on test passed on 2026-11-24** (issue #55): every key and the hook switch give the right codes, touch works by finger, all four lamps are right steady and blinking, the backlight steps and goes off, and handset and speaker calls to the echo service sounded right. The table below is from the day before, when most of that had only been checked by reading registers back.

**It runs on the phone, with `/data` in flash.** Started from Yealink's firmware by the unchanged loader, Linux 6.18.65 reaches the shell prompt about 10 seconds after the jump, and the whole system built against 6.18 headers (first stage, system image, `t48-shell` on the screen, the audio service, SSH) comes up on it. `reboot` works and lands in Yealink 55 seconds later.

| Part | How | Checked on the phone |
|---|---|---|
| Interrupts, timer, serial console | the kernel's own drivers (PL192, SP804, PL011), from the device tree | yes: `sleep 3` takes 3.00 s, 498.89 BogoMIPS as before |
| RAM | 118 MB of the 124 MB; the top 6 MB are the frame buffers | yes (115.6 MB free to use; the 144 KB just under the frame buffers are left out, as on 3.6) |
| GPIO | `drivers/gpio-bcmring.c`, with interrupts | yes: lamps through it, pen-down interrupt delivered |
| Screen | the kernel's simple framebuffer on U-Boot's picture; console and `/dev/fb0` | yes: the shell draws on it |
| Watchdog, reset | the kernel's SP805 driver with `patches/0002`; `t48-watchdog` uses `/dev/watchdog` | yes: fed by `t48-watchdog`; off for 50 s without a reset; a frozen feeder resets the phone within the 30 s; `back-to-yealink` and `reboot` reset |
| Key lamps | the kernel's `gpio-leds`, same names | by reading the pins back, not by eye |
| Holtek chip, message lamp | `drivers/t48s_holtek.c` | the chip answers and its watchdog is turned off; the lamp not by eye |
| Backlight | `drivers/t48s_backlight.c` | yes, by reading the register back |
| Keypad, hook switch | `drivers/t48s_keypad.c` | found running; test presses reach the shell; no real key pressed yet |
| Touchscreen | `drivers/t48s_touch.c` | the chip answers; a test touch opens Settings; no finger yet |
| Ethernet | `drivers/t48s_eth.c`, bound from the device tree | yes: DHCP, SSH, a 138 MB transfer |
| Audio | `drivers/t48s_audio.c`, bound from the device tree | recording yes (16000.9 frames/s, no errors); playback not tried |
| Flash | `drivers/t48s_nand.c`: reads everything, writes only `/data`'s range; UBI and UBIFS | yes: see [Flash](#flash) |
| U-Boot's BOOTMODE flag | `kernel/drivers/t48s_bootmode.c`, the same file as under 3.6 | reads `none`, as it should after a kexec start; a live start not tried |

Two more things were tried and work: the kernel's own `spi-pl022` and `ads7846` drivers find and talk to the touch chip, with pen-down as a GPIO interrupt (`dts/bcm11188-yealink-t48s-ads7846.dts` with `ads7846.config`). They are not the default because the input device then has another name and none of the test hooks our tools use.

DMA: both users (Ethernet on controller 1, audio on controller 0) program their channels directly, as on 3.6. The kernel's `dw_dmac` driver was not needed, and nothing else touches either controller now, so the 3.6 patches that kept the old platform code off controller 0 have no counterpart here.

Size: 2,147,794 bytes compressed with the first stage, every driver above, UBI and UBIFS, against the 2,621,440 of the kernel slot: 0.45 MB to spare. (1.60 MB for the kernel alone.)

## Flash

`drivers/t48s_nand.c` is the 3.6 driver ([docs/flash.md](../docs/flash.md)) on the current NAND layer, with the same rule: it reads the whole chip and can erase and program only 0x06a80000 to 0x07200000, the 60 blocks that hold `/data`. The range is compiled in. The same three layers each check the address themselves (chip commands, the NAND layer's write hooks, the MTD entry points); a page must read as erased before it is programmed, its error-correction bytes are computed in software, and it is read back and compared afterwards. No bad block table in flash.

One thing is new. **Every time it starts, the driver tries to break its own rule**: 167 attempts to erase or program places that are not ours (a block in every other partition, the blocks either side of our range, raw writes, spare-byte writes, commands it never sends), put to each layer separately, the lowest one as if the layers above had failed. Each must be refused with not one byte sent to the chip's command, address or data latches, which the driver counts. If any attempt gets through, there is no flash at all. The result is in the boot log and in `/sys/module/t48s_nand/parameters/selftest`.

UBI and UBIFS are configured as under 3.6 (LZO and zlib, two blocks reserved for bad ones) and with nothing 3.6 could not read: no zstd, no encryption, no authentication, no fastmap. `build.sh` checks.

Checked on the phone, 2026-11-23, against the `/data` that 3.6 made and has been using:

- before attaching, 6.18's read of the range matched 3.6's dump page for page, apart from the 11 pages 3.6 wrote after the dump;
- 6.18 attached it without writing anything (0 pages, 0 erases) and sees the same layout 3.6 does (60 blocks, 56 in use, 2 reserved, format w4);
- every file was identical: the SIP account, the SSH host key, the owner's keys;
- 6.18 wrote a 900 KB file (446 pages, 3 erases, no errors), and after a reboot **3.6 mounted the same filesystem with no recovery or complaint**, read that file back identical, and wrote one of its own; back under 6.18 both files and everything else were identical. The format stays w4: 6.18 does not upgrade it;
- a dump taken afterwards, spare bytes included, passes `tools/nandcheck.py ecc`: all 3480 written steps carry the right error-correction bytes, whichever kernel wrote them;
- from userspace, every other partition refuses to open for writing; the counters show nothing refused and nothing blocked beyond the one reset command at start-up.

A filesystem made fresh by 6.18 would be in a newer format (w5) that 3.6 can only read, so `t48-data format` asks for w4 while both kernels are in use.

## What is left before 3.6 can go (issue #55)

Done: the flash, the watchdog from userspace, the boot-mode reader, one root filesystem for both kernels, the hands-on test at the phone (keys, hook, touch, lamps, sound: 2026-11-24), and 6.18 as the default in CI, the release and `tools/ourkernel.sh` (2026-11-24).

Still needs someone at the phone:

- a cold start through U-Boot. Only the kexec start has been run on the phone. The live build exists (`LIVE=1`) and passes its checks on a PC; the test to run, with what to watch on the serial port, is "Cold-boot test for Linux 6.18" in [docs/live-image.md](../docs/live-image.md).

When the fallback goes, after one release: move `t48s_bootmode.c` here, build the root filesystem against 6.18 headers (`buildroot/configs/kernel6.fragment`; it is built against 3.6 headers until then so that one root filesystem runs on both), drop the `/dev/mem` half of `t48-watchdog`, and remove `kernel/`.

Found on the way, all fixed here: the kernel's unpacker ignores an appended device tree when it works out where RAM starts (`patches/0003`); this SoC's watchdog has its lock register at 0x400 and treats a load of 0 as 2^32 (`patches/0002`); the generic memory-mapped GPIO driver cannot share the pins with drivers that change them directly; programs built against 3.6 headers need `CONFIG_COMPAT_32BIT_TIME` or `sleep` returns at once.
