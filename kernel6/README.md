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

## Status (2026-11-23)

**It runs on the phone.** Started from Yealink's firmware by the unchanged loader, Linux 6.18.65 reaches the shell prompt about 10 seconds after the jump, and the whole system built against 6.18 headers (first stage, system image, `t48-shell` on the screen, the audio service, SSH) comes up on it. `reboot` works and lands in Yealink 55 seconds later.

| Part | How | Checked on the phone |
|---|---|---|
| Interrupts, timer, serial console | the kernel's own drivers (PL192, SP804, PL011), from the device tree | yes: `sleep 3` takes 3.00 s, 498.89 BogoMIPS as before |
| RAM | 118 MB of the 124 MB; the top 6 MB are the frame buffers | yes (115.6 MB free to use; the 144 KB just under the frame buffers are left out, as on 3.6) |
| GPIO | `drivers/gpio-bcmring.c`, with interrupts | yes: lamps through it, pen-down interrupt delivered |
| Screen | the kernel's simple framebuffer on U-Boot's picture; console and `/dev/fb0` | yes: the shell draws on it |
| Watchdog, reset | the kernel's SP805 driver with `patches/0002` | yes: `/dev/watchdog`, stops on close, `reboot` resets |
| Key lamps | the kernel's `gpio-leds`, same names | by reading the pins back, not by eye |
| Holtek chip, message lamp | `drivers/t48s_holtek.c` | the chip answers and its watchdog is turned off; the lamp not by eye |
| Backlight | `drivers/t48s_backlight.c` | yes, by reading the register back |
| Keypad, hook switch | `drivers/t48s_keypad.c` | found running; test presses reach the shell; no real key pressed yet |
| Touchscreen | `drivers/t48s_touch.c` | the chip answers; a test touch opens Settings; no finger yet |
| Ethernet | `drivers/t48s_eth.c`, bound from the device tree | yes: DHCP, SSH, a 138 MB transfer |
| Audio | `drivers/t48s_audio.c`, bound from the device tree | recording yes (16000.9 frames/s, no errors); playback not tried |
| Flash | `drivers/t48s_nand.c`, read-only, with `NAND=1` | yes: the whole chip read in 51 s, no bad blocks; every partition Yealink does not rewrite is identical, data and spare bytes, to the dump made under 3.6 |

Two more things were tried and work: the kernel's own `spi-pl022` and `ads7846` drivers find and talk to the touch chip, with pen-down as a GPIO interrupt (`dts/bcm11188-yealink-t48s-ads7846.dts` with `ads7846.config`). They are not the default because the input device then has another name and none of the test hooks our tools use.

DMA: both users (Ethernet on controller 1, audio on controller 0) program their channels directly, as on 3.6. The kernel's `dw_dmac` driver was not needed, and nothing else touches either controller now, so the 3.6 patches that kept the old platform code off controller 0 have no counterpart here.

Size: 2.01 MB compressed with the first stage and every driver above, against the 2.5 MB kernel slot (1.46 MB for the kernel alone).

Not done yet:

- Writing the flash (`/data` in UBI). This kernel cannot write it at all, so `/data` is in RAM.
- Userspace still reaches the watchdog through `/dev/mem`; it works, and can move to `/dev/watchdog` when 3.6 goes.
- Someone at the phone: real key presses, the hook, a finger on the screen, the lamps by eye, sound out.
- A cold start through U-Boot (only the kexec path has been run). U-Boot passes no device tree; the appended one is used.
- CI.

Found on the way, all fixed here: the kernel's unpacker ignores an appended device tree when it works out where RAM starts (`patches/0003`); this SoC's watchdog has its lock register at 0x400 and treats a load of 0 as 2^32 (`patches/0002`); the generic memory-mapped GPIO driver cannot share the pins with drivers that change them directly; programs built against 3.6 headers need `CONFIG_COMPAT_32BIT_TIME` or `sleep` returns at once.
