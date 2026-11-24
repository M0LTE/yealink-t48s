# The on-phone installer

Issue [#33](https://github.com/M0LTE/yealink-t48s/issues/33). The owner's side is [guide.md](guide.md). The install it performs is the one in [install.md](install.md); this page is what is new around it.

**Status, 2026-11-24: written, built and rehearsed off the phone. None of it has run on a phone.** What must be run on one is [at the end](#test-plan-for-a-real-phone).

A phone on Yealink's firmware is started from a USB stick ([live-image.md](live-image.md)). Settings then has an Install page. It backs the whole flash up to the stick, installs only when that backup has been checked, and puts the backup back on request. No computer, no network, no keys.

## The parts

| Part | Where | What |
|---|---|---|
| USB sticks in the kernel | [`kernel6/drivers/t48s_usb.c`](../kernel6/drivers/t48s_usb.c), `kernel6/usb.config` | the socket as a USB host, when U-Boot has started it |
| The engine | [`t48-install`](../buildroot/board/t48s/rootfs-overlay/usr/bin/t48-install) | status, backup, install, restore, restart |
| The page | `shell/settings.c`, Install | rows from `t48-install status`; its jobs run as a child with a progress bar |
| The app step | [`tools/phone/t48-flash.sh`](../tools/phone/t48-flash.sh), in the image as `/usr/lib/t48/t48-flash.sh` | the same script `tools/t48install.sh` copies over |
| The BOOTMODE flag | `t48s_nand_userenv_rewrite()` in [`kernel6/drivers/t48s_nand.c`](../kernel6/drivers/t48s_nand.c) | the one block of `userenv` |
| The stick | `t48s-stick.zip` from CI | the live set, and `install/` |

## The stick

```
T48S.rom  T48S.bin  T48S.rfs  README.txt      what U-Boot starts (live-image.md)
install/T48S-kernel.uimg                      goes into the kernel partition
install/system.img                            goes into system slot A
install/SHA256SUMS
t48s-backup/                                  made on the phone
```

`t48-install` takes the first FAT partition of a USB disk that holds `T48S.rom` and mounts it on `/run/t48-stick`.

## USB

The socket is the SoC's USB controller 1: a standard EHCI whose registers start at 0x80410800, interrupt 18. Both are read from U-Boot's code (`ehci_hcd_init` at 0x03B24EC4) and Linux 3.6's `mach-bcmring` headers. The kernel's own `ehci-platform` driver runs it, with USB storage, an MBR partition table and FAT. The flash kernel still fits its partition: 2,303,174 of 2,621,440 bytes.

**Only after a start from a stick.** Clocks, resets, the PHY and two pins are set up by U-Boot when it reads a stick, and by nothing else. `t48s_usb` reads three chip-control registers and lets the EHCI driver in only if the USB host's bus clock, the USB clock and host mode are all on. On any other start it says `USB is off` and touches nothing: a read of a block with no clock stops this SoC, as audio showed. U-Boot's sequence is written out in the driver's header for whoever does the cold start. The installer does not need it.

## The backup

`t48-install backup`, about 10 minutes, 138 MB:

1. Each of the twelve partitions is read with its spare bytes (`nandread -o`) straight onto the stick, and its SHA-256 taken on the way. A bad block or a read error stops it.
2. The page cache is dropped and each file read back from the stick: same SHA-256.
3. Each partition is read from the flash a second time: same SHA-256.
4. `nandrestore -n` recomputes every error-correction byte of each file: a restore would write exactly these bytes.
5. `SHA256SUMS`, then `VERIFIED` (the SHA-256 of `SHA256SUMS`, and the date). No `VERIFIED`, no install.

The folder has the same files as `tools/nandbackup.sh take` makes, so `tools/nandbackup.sh check` and `tools/t48install.sh restore --other-phone` take it on a PC.

A backup that was cut short is renamed `t48s-backup.partN` and a new one made. A finished backup is never overwritten. A finished backup of another phone stops everything until it is moved.

## Install and restore

Both begin the same way and stop at the first thing that is not right:

- the phone was started through recovery mode (`t48boot=live`), so nothing of the flash is in use;
- `VERIFIED` matches, and every backup file matches `SHA256SUMS`;
- ubl, uboot, env, logo, smkern, smrfs and rfs on the phone are identical to the backup, all 2112 bytes of every page: it is this phone's backup. `cfg` and `data` may differ (Yealink ran since) and are then compared with how they were when the step began;
- the flash driver's self-test ran, it has counted no write errors, and `kernel` and `app` have no bad block.

**Install**: the install files match their `SHA256SUMS`. The kernel lock is opened, `T48S-kernel.uimg` written and compared page by page with its error-correction bytes (`nandrestore -b`), and the lock closed. Then the app step: all 599 blocks of `app` are erased (539 on a phone whose last 60 already hold our `/data`), UBI is made, slot A written and read back, an empty `/data` made. Then every partition that is never written is compared again.

**Restore**: `app` from the backup, then `kernel`, each page with its spare bytes, compared afterwards. `kernel` goes last: a restore cut short leaves a phone that still starts our kernel and shows rescue.

Both end by looking at U-Boot's BOOTMODE flag and clearing it if it is still set. If it cannot be cleared the last line says `Take the stick out before restarting`.

Found by the rehearsal: a new, empty UBI volume has no filesystem and cannot be mounted read-only, which is what the app step did next on a phone with no earlier `/data`. It now mounts it for writing once.

## The BOOTMODE flag: the one write outside kernel and app

Why it matters is in [live-image.md](live-image.md#leaving-the-phone-as-it-was-the-bootmode-flag). `kernel/drivers/t48s_bootmode.c` builds the new 4 KB block in the kernel and takes one word from userspace. The 6.18 flash driver now provides the write it asks for:

- **Not a fourth range.** A window, closed at every start and outside `t48s_nand_userenv_rewrite()`. Open for erase it passes one erase of the one block named; open for program it passes pages 0 and 1 of that block. Nothing but a whole block of `userenv` can be named ([`t48s_nand_userenv.h`](../kernel6/drivers/t48s_nand_userenv.h)).
- **The same three layers** ask the window for themselves, and each page gets the erased check, software error correction and raw read-back of every other write. A bad block marker can never go through it: those paths ask the allow-list alone.
- **Refused**: a phone not started through recovery mode; any length but 4096; a block whose own CRC is wrong; a third call in one boot.
- **Self-test.** The start-up test gains a pass: 549 attempts and questions in all (396 before). For the block the window names it only puts the questions, never a write; every other block is tried at all three layers with the window open in each phase.
- **On a PC.** `tools/test-userenv.sh` runs the window's rule against every page of the chip in every state, 16.6 million checks, against a separate statement of the rule.

If the rewrite fails after the erase, `userenv` is empty. U-Boot then uses its built-in values. The variables it held are in the kernel log and in `mtd3-userenv.dataoob` of the backup.

## Rehearsed

```
tools/test-owner.sh bytes SET BACKUP      # any PC, 30 seconds
sudo tools/test-owner.sh ubi SET          # Linux with nandsim
tools/test-userenv.sh
make -C shell check                       # 11-install.script
```

| | How |
|---|---|
| Backup: twelve files byte for byte the flash; checksums; a second backup keeps the first | `bytes`: the phone's own `t48-install` against a flash in a file made from Tom's backup |
| Refused, with nothing written: no backup, an unfinished one, a damaged one, another phone's, damaged install files, a phone not started from a stick | `bytes` |
| Install: the kernel partition is exactly what `tools/t48flash.py` works out, U-Boot's read of it gives the file back, 599 blocks of app erased, nothing else changed | `bytes` |
| Restore: the whole flash is the backup again, byte for byte | `bytes` |
| The app step on a phone that never had our `/data`: 599 blocks of somebody else's data, UBI from nothing, slot A, an empty `/data` that mounts, a second run changes nothing, the first start loads slot A | `ubi`, on studybox |
| The page: greyed Install, the backup's progress, the question, a failure's words, the way back | simulator, against a stand-in for `t48-install` |
| Kernels with USB and the userenv window build; the flash kernel fits | `kernel6/build.sh` with `LIVE=1` and `FLASH=1` |

## Test plan for a real phone

Serial log running throughout (`ssh root@proxmox0 tail -f /root/t48s/session4.log`). A proper power supply. Tom's backup at hand.

**A. On the installed phone, nothing written** (needs the stick put in and one recovery start):

1. Build the stick from this branch (`mkliveimage.py usb`, `install/`, `mkusbstick.sh`). Speakerphone, 2.
2. Serial: `t48s_usb: chip control as found: ...` (keep the line), `USB host is running`, `ehci-platform ... EHCI`, `usb 1-1: new high-speed`, `sd 0:0:0:0: [sda]`. If the kernel stops after `chip control as found`, the guard let it in too early: keep the line.
3. `t48s_nand: self-test: 549 attempts`. Any other number: stop.
4. `t48-live status`: `bootmode_was=usbmode`, `bootmode_now=none`, `clear=BOOTMODE cleared`. Serial shows the variables kept and `userenv: rewritten and read back`. **This is the first write to `userenv` by our kernel.** Then, from a PC, `tools/nandbackup.sh verify` against Tom's backup: only `userenv`, `kernel` and `app` may differ.
5. `t48-install status`: `stick=yes`, `flash=ours`.
6. A TFTP start (menu 1) of the same kernel: `USB is off`, and the kernel carries on.

**B. Restore, then the stranger's path, on the same phone** (it ends installed again):

7. Copy Tom's backup onto the stick as `t48s-backup`, with a `VERIFIED` made by hand. Settings, Install, Restore Yealink firmware. Time it. `tools/nandbackup.sh verify` from the stick start: every partition but `userenv` identical to the backup.
8. Stick out, power on: Yealink's firmware, settings intact.
9. From Yealink: stick start, Settings, Install, Back up. Time it. Compare the stick's backup with Tom's on a PC.
10. Install. Time it. Restart. Our firmware from flash, `/data` empty and writable.
11. A power cut during the app step (pull the plug at "Writing the system"), then Install again.

**C. Another phone** (the T46S tester's, or a T48S on newer Yealink firmware):

12. Does its U-Boot show the same menu and take the same `T48S.rom`?
13. Bad blocks: `nandread -b` on every partition. A phone with one anywhere stops the backup today.
14. The last 60 blocks of `app`: erased, as on Tom's, or in use? (Install erases them either way.)
15. A stick other than the SanDisk Ultra Fit.

Not covered anywhere yet: a stick pulled out during the backup, and a FAT filesystem that fills up.
