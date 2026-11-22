# Our kernel for the T48S

**Status (2026-11-22): boots to a busybox shell on the serial console.** U-Boot's recovery mode (option 1, TFTP) loads it with our ramdisk; it prints the full boot log on ttyAMA0, mounts the ext2 ramdisk, runs our init, draws a banner on the screen through `/dev/mem`, and gives a working shell (`t48s#`). Timekeeping is right (`sleep 10` takes 10.007 s); BogoMIPS reads 331.77 against Yealink's 498.07, which is the delay-loop measurement, not the clock. No drivers yet for Ethernet, screen, touch, keypad, NAND or audio.

Linux 3.6.11 (the last mainline release with Broadcom's `mach-bcmring` platform, the BCM11107 family the phone's BCM11188 belongs to), patched and configured for the phone. Built with gcc 4.7 in an Ubuntu 14.04 container.

- `build.sh`: applies the board patches and builds `T48S.bin` (U-Boot legacy image, load and entry 0x0E000000 like Yealink's kernel). Run inside the container from a directory holding `linux-3.6.11/` and this `kernel/` directory:
  `docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w t48s-kbuild:trusty sh kernel/build.sh`
- `t48s.config`: config added on top of `bcmring_defconfig`.

Container (on hyperv-gha, `~/t48s-kbuild`):

```
FROM ubuntu:14.04
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gcc gcc-arm-linux-gnueabi make bc u-boot-tools xz-utils bzip2 patch
```

Board changes:

| What | Mainline | T48S |
|---|---|---|
| Machine number | 2289 | 2342 (what U-Boot passes) |
| RAM | 256 MB from 0 (forced by the platform fixup, which also drops U-Boot's tags and command line) | 0x00400000 to 0x07A20000, stopping below U-Boot's framebuffer at 0x07A24000 |
| UART clock | 142.857 MHz | 100 MHz, measured from the divider U-Boot programmed |
| PLL spread spectrum | enabled at boot | left as U-Boot set it |
| Flash (MTD/NAND) | built in | not built: this kernel cannot write the NAND |
| Command line | from U-Boot | built in: serial console on ttyAMA0 at 115200, ramdisk at 0x02a80000 |
| Early debug output | none for this platform | `mach/debug-macro.S` for UARTA (phys 0x80012000, virt 0xF8012000) |
| Static I/O map | UMI and NAND entries overlapping CHIPC | dropped (3.6 BUG()s on overlaps) |
| UART clock lookup | registered for `uarta`/`uartb`, devices named `uartA`/`uartB` | names fixed; use count starts at 1 so the AMBA ID read does not gate the clock off under the console |

Pitfalls found on the way, in boot order: RAM must end on a 1 MB boundary or 3.6 hangs in `paging_init`; overlapping static mappings hit `BUG()` in `vm_area_add_early`; a UART clock gated off during `amba_device_add` leaves the early console spinning on a full FIFO (a silent hang).

Userspace for testing: `tools/mkrfs-bb.sh` builds `T48S.rfs` with a static busybox (musl), a shell on the serial console and `native/membanner` drawing on the screen. Boot it with U-Boot's recovery mode, option 1 (TFTP), served by `tools/tftpd.py DIR 69 --rom-once`.
