# What we keep in flash

Issue [#29](https://github.com/M0LTE/yealink-t48s/issues/29). The design is in [architecture.md](architecture.md#flash-layout); how the chip is read and its error correction are in [nand.md](nand.md); the backup and the way back in [backup.md](backup.md).

## Today: `/data`, beside Yealink's firmware

| | |
|---|---|
| Where | the last 60 blocks of the `app` partition: 0x06a80000 to 0x07200000, 7.5 MB. MTD partition `t48data` (`/dev/mtd13`) |
| What | UBI, with one volume, `data`, holding UBIFS, mounted on `/data` |
| Holds | settings and the SIP account (`/data/config`), SSH host key and the owner's keys (`/data/access`), installed apps, call history, a random seed, the owner's own changes (`/data/local`) |
| Room | about 5 MB, compressed as it is written |

Why there and not the whole `app` partition, as the design has it: **the phone cannot yet start without Yealink's firmware.** Our kernel is started from Yealink's running system (kexec), and it relies on Yealink having set up the audio hardware and the Ethernet switch first. The hook that lets us in lives in `app` itself (`/phone/scripts/phone.sh`). Turn all of `app` into ours today and the phone would boot to a broken Yealink with no way in but recovery mode.

Those 60 blocks are the part of `app` Yealink never used: its filesystem fills blocks 0 to 538, mounts read-only, and reports the rest as "60 erased blocks". It decides that from the spare bytes of each block's first page, which UBI never writes, so it goes on seeing 60 erased blocks and never looks inside them. Checked on the phone after writing (below).

So this step keeps everything of Yealink's, loses nothing we want later (UBI grows downwards over the rest of `app` when that day comes: it does not record block numbers), and is undone by erasing 60 blocks.

```
t48-data status        # flash or RAM, how full, bad blocks, wear
t48-data format        # once: set the flash up (only onto erased flash) and copy /data over
t48-data release       # stop using the flash; /data carries on in RAM
```

At boot `rcS` runs `t48-data mount`: if our blocks hold UBI, `/data` comes from flash; if not (a phone that has not been set up, or a kernel without our partition) it is in RAM as before, and the console says so. Nothing is ever formatted by itself.

## What the driver will write, and what it will not

`kernel/drivers/t48s_nand.c` reads the whole chip and writes **only the ranges compiled into it**; today that is the one above. A range must lie inside `kernel` or `app` or the driver refuses to start. The loader, U-Boot, both environments, the logo and the safe-mode kernel and root filesystem can never be written; nor, today, can anything else of Yealink's.

Three layers each check the address for themselves:

1. **Chip commands.** Program and erase commands are held back until their address bytes have arrived, and sent only if the address is in a range. Lock, reset and unknown commands are never sent.
2. **The write code** checks the page before it sends anything. There is no raw write, and the only write of spare bytes alone is a bad block marker.
3. **The MTD layer**: every partition but ours is registered read-only, and the device refuses writes and erases outside the ranges.

Every page written:

- must read as erased first, or it is refused: a page is programmed once;
- gets its error-correction bytes computed in software, with the same code `tools/nandcheck.py` uses, which matches every written page of the stock flash. The driver checks that code against known answers when it starts and gives no flash at all if it is wrong;
- is read back raw, all 2112 bytes, and compared. Any difference is an error.

Bad blocks: skipped, and a block that fails is marked the way U-Boot and Yealink expect (spare byte 0 of its first page). There is no bad block table in flash and none is ever made. UBI does the skipping, moving and marking; it keeps two blocks in reserve, and `t48-data format` leaves four more unallocated.

Counters: `/sys/module/t48s_nand/parameters/` has `pages_written`, `blocks_erased`, `write_errors`, `refused` (writes outside our ranges) and `blocked` (chip commands not sent).

The kernel has UBI and UBIFS and still no other way to the flash: no block devices, no other flash filesystems, no partitions from the command line, not mainline's driver; `kernel/build.sh` stops if any appears. The image has busybox's UBI tools and no raw flash tools.

## Next: starting without Yealink

The rest of the layout (our kernel in the `kernel` partition, all of `app` as UBI with two system slots) waits on the phone starting cold. In order:

1. **Bring the hardware up from cold.** The audio block's start-up sequence is known from Yealink's code but untested ([audio.md](audio.md)); the Ethernet switch needs its ports and address table set up. Both can be worked on through recovery-mode boots (U-Boot loads our kernel from TFTP: no flash involved), which need someone at the phone.
2. **A kernel that fits.** Kernel plus first stage must be under 2.5 MB compressed for the `kernel` partition; the first stage is about 1 MB before compression ([boot.md](boot.md)).
3. **The first stage finds the system image in flash**: a UBI volume per slot, read into RAM and mounted the same way as today.
4. **Then write**: add `kernel` and all of `app` to the driver's ranges, restore-test them with `nandrestore`, write a kernel that has already booted through recovery mode. Recovery mode and safe mode stay as the ways back.

One thing to settle before step 4: what Yealink's own updater does with an `app` partition full of data it does not know, since that is the stock way back for other people's phones.
