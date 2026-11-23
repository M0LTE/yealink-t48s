# The NAND flash from our kernel

Issue [#5](https://github.com/M0LTE/yealink-t48s/issues/5). **Read-only.** Nothing here writes, erases or locks the flash, and the kernel is built so that it cannot. Writing is a later step that needs Tom's go-ahead (architecture.md, open question 4); the last section lists what it will take.

## Summary

RESULT_SUMMARY

## The chip and how it is used

Winbond, ID `ef f1`, 128 MB, 2048-byte pages each with 64 spare bytes, 128 KB erase blocks (1024 of them). It hangs off the SoC's UMI block (registers at 0x80001000, the chip's command, address and data latches at 0x80001800), which has a hardware error-correction engine.

Yealink's kernel uses Broadcom's `bcm_umi_nand` driver, the same one that is in mainline 3.6, but not with mainline's settings:

| | Phone (U-Boot and Yealink) | Mainline 3.6 default |
|---|---|---|
| Correctable bits per 512 data bytes | 4 | 8 |
| ECC bytes per 512 data bytes | 7 | 13 |
| Free spare bytes per page | 35 | 2 |

Mainline as shipped would not read this flash. The setting was read from the controller's registers as Yealink left them (n = 4152 bits, k = 4100, t = 4) and confirmed against the flash contents.

Each page's 64 spare bytes are four groups of 16, one per 512 bytes of data:

| Bytes | Use |
|---|---|
| 0 | bad block marker (first group; free in the other three) |
| 1 to 8 | free. Yealink's yaffs2 keeps its tags here. Not covered by the ECC |
| 9 to 15 | ECC for that 512 bytes |

The code is an ordinary BCH code and can be computed in software: GF(2^13), primitive polynomial 0x201b, 52 parity bits; data bits most significant first, then four zero bits; parity stored big-endian in the low 52 bits of the 7 bytes. `tools/nandcheck.py ecc` does that for a dump. Run over the whole flash as dumped under Yealink: all 207,828 written 512-byte steps match, none differ. So any dump with spare bytes can be checked on a PC without the phone, and so can an image before it is ever written.

A page that was never written reads as all `ff`, ECC included. There is no bad block table in flash: both kernels scan the markers at boot. This phone has no bad blocks.

U-Boot and Yealink's kernel read each other's writes (U-Boot writes `env` and `userenv`, Linux writes everything else, U-Boot loads the kernel Linux wrote), and all of those partitions carry the same code, so one layout serves all three.

Yealink's yaffs2 partitions (`rfs`, `smrfs`, `app`, `cfg`, `data`) are an encrypted variant ("yaffs image encrypted!" in its boot log). We cannot mount them, and do not need to.

## Our driver

`kernel/drivers/t48s_nand.c`: mainline's driver cut down to reading, with the phone's ECC settings. No DMA, no interrupts. Partitions as Yealink numbers them, so `/dev/mtd7` is the kernel under both; `mtd9` (an empty "boot" under Yealink) is the whole chip under ours.

It cannot change the flash. Three layers, each enough on its own:

1. **Chip commands.** Every command byte goes through one function with a whitelist: read page, random data output, read ID, read parameter page, read status. Program, erase, lock, reset and anything unknown are dropped and counted in `/sys/module/t48s_nand/parameters/blocked`.
2. **No write code.** Every write hook the NAND layer offers (page write, spare write, erase, mark bad, raw data out) is a stub that sends nothing. The driver refuses to register the flash if any hook is not its stub.
3. **Read-only to the rest of the kernel.** The device and every partition are registered without the writeable flag, so the MTD core answers "read-only file system" to write, erase and mark-bad, and `/dev/mtdN` cannot be opened for writing.

Around that: the kernel has no flash filesystems, no UBI, no MTD block devices and no partitions from the command line, and `kernel/build.sh` stops if any of them is configured, or mainline's driver. The root filesystem has no mtd-utils. The only flash tool in it is `nandread` (`native/nandread.c`), which opens the device read-only and uses read calls only.

The driver leaves the controller's timing, chip select and write-protect settings as it finds them. It drives only the ECC engine.

## Dumps and comparison

```
tools/ourkernel.sh ssh 'nandread -o /dev/mtd7' > kernel.dataoob     # data + spare bytes
tools/ourkernel.sh ssh 'cat /dev/mtd7' > kernel.bin                 # data only
tools/nandcheck.py ecc kernel.dataoob                               # ECC check on a PC
tools/nandcheck.py cmp a.dataoob b.dataoob                          # two dumps, page by page
```

`nandread` also runs under Yealink's kernel (copy it to `/tmp`, use `/dev/mtdNro`), which is how the reference dumps were taken.

RESULT_COMPARE

## What writing safely will need

Not done, and not to be done without Tom's go-ahead. Input for [#29](https://github.com/M0LTE/yealink-t48s/issues/29), [#30](https://github.com/M0LTE/yealink-t48s/issues/30) and [#8](https://github.com/M0LTE/yealink-t48s/issues/8).

- **ECC: settled.** Writes must use the phone's layout above (hardware engine, 7 bytes per 512, at spare bytes 9 to 15). Mainline's write routine, by its code, produces exactly that when built for 7 ECC bytes. Anything written can be read back raw and checked against the software code before it is trusted.
- **U-Boot must read the kernel we write.** Same ECC, so yes in principle. Still to check in U-Boot's code: how its `nboot` treats a bad block inside the kernel partition (skip, or fail). This phone has none, others will.
- **Bad blocks.** Marker in spare byte 0 of a block's first page; no table in flash, and we must never create one (Yealink's kernel and U-Boot do not expect it). A writer must skip marked blocks and must be able to mark a block that fails.
- **Backups must include the spare bytes.** Yealink's yaffs2 keeps its tags there, so a plain `cat /dev/mtdN` of `app`, `rfs`, `cfg` or `data` cannot be restored into a working Yealink system. `nandread -o` output can. A restore tool has to write data and tag bytes together and let the hardware add the ECC. Recovery mode with a stock firmware file remains the way back that needs none of this.
- **Partitions the design reuses** (architecture.md): `kernel` (2.5 MB; Yealink's kernel fills 1.8 MB of it) and `app` (75 MB, 599 blocks) for UBI. UBI does not use spare bytes, which suits this layout; with 2048-byte pages and no sub-page writes a block holds 124 KB of payload, so about 70 MB after UBI's reserve. Kernel 3.6 has UBI; its ECC needs (4 bits per 512 bytes on this SLC chip) are met. `ubl`, `uboot`, `env`, `userenv`, `logo`, `smkern`, `smrfs` stay untouched for ever; in a writing driver they should stay registered read-only, as now.
- **A write path in the driver**, opened one partition at a time: the command whitelist grows by program and erase only for the partitions we own, checked by address, so a bug elsewhere still cannot reach the loader or U-Boot.
- **The write-protect pin.** The controller can hold the chip's write-protect line (bit 15 of its control register at 0x8000100c; U-Boot and Yealink leave it released). Whether the line is wired on this board is untested. If it is, holding it whenever we are not writing is a cheap fourth layer.
- **First write.** After a full backup with spare bytes has been checked with `nandcheck.py` and copied off the phone: one block of `app`, erased, written, read back raw, compared. Then the kernel slot, with the safe-mode kernel and recovery mode as the ways back.
