# Running our own kernel: research

Research done 2026-11-22. Nothing was written to flash; everything below comes from reading the phone (flash partitions through the read-only `/dev/mtdNro` nodes, `/proc`), the U-Boot binary, and the web. Dumps are in `dumps/` (not committed: vendor code).

## Summary

Running our own kernel looks quite achievable. There is no signature check anywhere we have found:

- U-Boot loads the kernel from flash as a plain U-Boot "legacy" image and checks only a CRC32. Both kernels in flash (main and safe mode, byte-identical) are ordinary gzip zImages.
- U-Boot has a built-in **recovery boot from a USB stick or TFTP** that loads a kernel (`<name>.bin`) and ramdisk (`<name>.rfs`) and runs them, after checking a `.rom` file whose checks are CRC, magic and version fields, not a cryptographic signature. That should let us boot our own kernel from a USB stick **without writing flash at all**. It is reached by holding a particular key at power-on (which key is not yet known; see below).
- The serial port works in both directions at the hardware level. Yealink's software ignores input (U-Boot and Linux both), but our own kernel would have a normal interactive serial console.
- A kexec-style kernel module is a second zero-flash route, entirely from the running system: the module format and all the symbols it needs are available.

The hard part is not booting a kernel, it is drivers. Yealink has not published the source for this phone's kernel, and mainline Linux dropped this chip family in 2012. A kernel of ours would start with serial, RAM, timers and probably NAND, and the screen, touch, keypad, Ethernet and audio would all need drivers written or ported.

## The boot chain

| Stage | What it is | Notes |
|---|---|---|
| Boot ROM | in the SoC | loads UBL from NAND |
| UBL 1.1.0.8 | `mtd0`, 128 KB | loads U-Boot |
| U-Boot 2.5.3.0-T48S | `mtd1`, legacy image, load 0x03B00000 | git `308d13e-dirty`; talks to the Holtek chip; serial input ignored |
| Kernel | `mtd7` at 0x015A0000 | legacy image, CRC32 only, gzip zImage, Linux 2.6.27.47, Wind River gcc 4.3.2 |
| Safe-mode kernel | `mtd5` at 0x00420000 | identical to `mtd7`; boots with root on `smrfs` (`run bootram`) |

U-Boot environment (`mtd2`), the important parts:

```
bootcmd=run bootnand
bootdelay=1
bootnand=... nboot 0x02680000 0 0x015A0000           (normal boot)
bootram=...  nboot 0x02680000 0 0x00420000           (safe mode: smkern + smrfs)
boottftp=... tftp 0x02680000 ${phonename}.bin; tftp 0x02a80000 ${phonename}.rfs; bootm 0x02680000
bootstorage=... usbstart; fatload usb ${usbpart} 0x02680000 ${phonename}.bin;
             fatload usb ${usbpart} 0x02a80000 ${phonename}.rfs; bootm 0x02680000
```

`boottftp` and `bootstorage` boot with `initrd=0x02a80000,0x800000 root=/dev/ram0 rw rootfstype=ext2`: an ext2 ramdisk of up to 8 MB. `bootconsole` is not set, which is why Linux prints nothing on the serial port.

The kernel's machine number (passed by U-Boot in r1) is **2342**, not mainline's `MACH_BCMRING` (1547). Board name `BCMRING_SV`.

## The keypad at power-on

U-Boot's key scan (code at 0x03B15910) polls the key 20 times, 100 ms apart, printing `scan key=N`. If the same key is still held at the end it returns:

| Key held about 2 s | Scan code | Result | What U-Boot does |
|---|---|---|---|
| Speakerphone (Yealink's documented recovery key; older guides say Redial, which is probably the other of 40/41) | 40 or 41 | 1 | "Start Updating...": the recovery updater (network setup, `.rom` check, then the USB/TFTP boot below) |
| ? | 50 | 2 | (handled inside the updater: selects USB) |
| ? (avoid) | 60 | 3 | 0x03B1522C: erases the `cfg` partition (`/config`) and sets `NeedSync`, i.e. a configuration reset that would also remove our boot hook |
| OK | 16 | 4 | ignored at boot; inside the updater it runs the same `cfg` reset, so do not hold OK at the recovery menu |

Known codes from Tom's test: OK 16, X 17, \* 10, # 11. Codes 40/41, 50 and 60 are other keys, not yet identified.

The updater fetches `<phonename>.rom` (USB or TFTP), checks it (`Rom crc Err`, `Rom magic Err`, `Rom type Err`, `Rom hwid`, `Rom swid` not lower than installed, `Rom OEM Err`), prints `Rom check ok`, sets `BOOTMODE=usbmode` and runs `bootstorage` (or `resumemode` and `boottftp`). The `.bin` it then boots is normally Yealink's updater kernel; U-Boot checks only its image CRC. A genuine Yealink `.rom` (we have 66.82.0.31, magic `ad24ec0b`, same as installed so not "lower") should pass the ROM check, and the `.bin` and `.rfs` next to it can be ours.

Still unknown: the exact `phonename` (the code has 'T2x', 'T61', 'T66', 'T41', 'T42' nearby; the serial log will show it), and whether anything is written to flash before the `.bin` runs. Everything seen so far only reads, but this needs confirming in the code at 0x03B15274 before trying it with a real ROM on the stick.

## The running kernel

- No `/proc/config.gz`. `sys_kexec_load` is only a stub: no built-in kexec.
- Modules: `vermagic=2.6.27.47 preempt mod_unload ARMv6`, no symbol version CRCs, `struct module` is 0x140 bytes. A module built against vanilla 2.6.27.47 with those options should load. `keypad.ko` and `tsc.ko` declare `license=GPL`.
- `/proc/kallsyms` (6830 text symbols) includes what a kexec-style module needs: `setup_mm_for_reboot`, `v6_flush_kern_cache_all`, `cpu_v6_proc_fin`, `cpu_v6_reset`, `machine_restart`, `module_alloc`.
- Memory: RAM 0x00400000 to 0x07FFFFFF (124 MB), framebuffer reserved at 0x07A24000. UARTs are PL011 at 0x80012000 (`ttyAMA0`, the CON4 header) and 0x80013000; NAND controller at 0x80001800; hardware watchdog at 0x80017800; SPI at 0x80015000.

## Serial console

CON4 is `ttyAMA0`. Phone to PC works. PC to phone: the UART's receive counter in `/proc/tty/driver/ttyAMA` counts every byte we send, but neither U-Boot nor Yealink's Linux passes them on. So the hardware path is fine, and our own kernel should get a working two-way console. (Linux's console is silent today only because `bootconsole` is empty.)

## Watchdogs: the main risk

- The keypad driver talks to the Holtek chip, which has its own watchdog (`/proc/keypad/watchdog` reads 11; driver functions `holtek_get_watchdog`, `holtek_set_watchdog`). Freezing Yealink's UI rebooted the phone within minutes, so something keeps it fed.
- The SoC watchdog at 0x80017800.

If either is armed when our kernel takes over, the phone will reset after some seconds. The first boot experiment will show it; a reset would mean the new kernel has to feed it (Holtek protocol to be worked out from `keypad.ko`) or turn it off.

## Source code

- Yealink's GPL page offers a "Linux Kernel" archive for "T4XS, ..." but it is a RAR of a **MontaVista Linux 2.6.10 MIPS tree** from 2010, not this phone's 2.6.27 ARM kernel. No U-Boot source is published. A GPL source request to Yealink is possible (the kernel and `keypad.ko` are GPL) but is Tom's call.
- Mainline Linux had `arch/arm/mach-bcmring` (BCM11107) from about 2.6.31 until it was removed for 3.7 (2012): timers, interrupts, DMA, clocks, PL011 serial and the `bcm_umi_nand` NAND driver; no Ethernet, LCD or keypad. Never device tree. v3.6 is the last tree with it.

## USB boot smoke test (prepared 2026-11-22)

U-Boot details that settle the file names: `phonename` is set by U-Boot from the board type, `T48S` here, so it wants `T48S.bin` and `T48S.rfs`. The ROM name is `phonename` unless an internal board-type byte maps to 'T2x', 'T61', 'T66', 'T41' or 'T42', so the stick carries the ROM under both names. `usbpart` is set by U-Boot's USB detect (first device:partition with a filesystem). In the updater, a board-type check either goes straight to USB or shows an on-screen menu (network or USB).

`BOOTMODE`: U-Boot saves `BOOTMODE=usbmode` before booting from USB. Yealink's `rcS` reads `BOOTMODE` once and clears it (`userenv -s BOOTMODE`), then runs `usbmode.sh` (reflash `T48S.rom` from the stick), `resumemode.sh` (reflash over TFTP), `safemode.sh` or `normode.sh`. So a boot that skips Yealink's `rcS` must clear it itself, or the next normal boot runs the USB reflash once instead of the phone.

The stick (FAT32, plugged into the phone) now holds:

| File | What |
|---|---|
| `T48S.rom`, `T66.rom` | genuine 66.82.0.31 firmware, only to pass U-Boot's ROM check |
| `T48S.bin` | Yealink's own kernel, byte for byte from `mtd7`, so this test only proves the boot route |
| `T48S.rfs` | our 4 MB ext2 ramdisk (`tools/mkrfs.sh native/usbinit`) |

`native/usbinit.c` (the ramdisk's `/sbin/init`): clears `BOOTMODE` with Yealink's own `userenv`, prints `/proc/cmdline` and `/proc/version`, draws "BOOTED FROM USB" on the screen through `/dev/mem` at 0x07A24000, echoes anything typed on the serial port, and prints `alive N s` every second. Exact files and checksums are in `dumps/usbtest/` (not committed).

To run it (Yealink's documented procedure, "Recovery Mode on Yealink IP Phones", USB section for T41S/T42S/T46S/T48S): hold the **Speakerphone** key and reconnect power, keep holding until the screen shows "1. TFTP / 2: USB", then press **2**. Do not hold OK. Success looks like "BOOTED FROM USB" on the screen and `alive` lines in the serial log on proxmox0. If the counter stops and the phone reboots, that is a watchdog, and the last number is its timeout. Afterwards, power-cycle without holding anything for a normal boot.

Risk notes: nothing is written to flash except `BOOTMODE` in the userenv partition (by U-Boot, then cleared by our init, as Yealink does every boot). If our init failed to clear it, the next normal boot would run `usbmode.sh`, which reflashes 66.82.0.31 from the stick (the version already installed); our files in `/config` and `/data` survived the earlier web upgrades, so they should survive this too.

## Paths to our own kernel

1. **Boot from USB via the recovery mode (no flash writes).** Needs: the key for scan code 40/41 (and maybe 50 for USB), a FAT USB stick with the genuine `.rom` plus our `.bin` (legacy image) and `.rfs` (ext2 ramdisk), named after `phonename`. First target: a minimal kernel with serial console and a busybox ramdisk, reaching a shell on CON4. Kernel options: mainline v3.6 `bcmring_defconfig` adapted to machine 2342 and this board's memory map; it covers serial and NAND. Build needs an old toolchain (gcc 4.x era), in a container or on a build box.
2. **kexec-style module (no flash writes, no keypad).** A module for the running kernel that copies a zImage into RAM, shuts down caches and MMU with the kernel's own helpers and jumps to it (what ARM's `machine_kexec` does). Original work: no existing ARM kexec-as-module project was found. Fully remote, so good for fast iteration once written.
3. **Write a kernel to the safe-mode slot (`mtd5`).** The safe-mode kernel is a spare copy; writing ours there and selecting it (via the safe-mode boot or the U-Boot environment) leaves the main kernel untouched. Needs an MTD write tool (mtd-utils source is on Yealink's GPL page) and care with the NAND ECC layout. Riskier, and only worth it once paths 1 or 2 have proven a kernel boots.

Replacing Yealink's drivers (screen, touch, keypad, Ethernet, audio) is the long part whichever path boots the kernel. The 800x480 screen is a plain framebuffer at a fixed address set up by U-Boot, so a kernel can probably keep drawing to it with a simple framebuffer driver before any proper display driver exists.

## What Tom can do to move this on

1. **Run the USB boot smoke test:** Speakerphone held while reconnecting power, then 2 at the menu.
2. ~~A FAT-formatted USB stick~~ done: plugged in and prepared, see "USB boot smoke test".
3. Decide whether to ask Yealink for the 2.6.27 GPL source.
