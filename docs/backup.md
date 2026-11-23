# Backing up the flash, and going back to stock

Issue [#30](https://github.com/M0LTE/yealink-t48s/issues/30). Do this before anything of ours is written to a phone. How the flash is laid out and read is in [nand.md](nand.md); what we write is in [flash.md](flash.md).

## Take the backup

With the phone running our kernel (a kexec boot, nothing written yet) or Yealink's firmware with SSH:

```
tools/nandbackup.sh take DIR        # about three minutes
```

It copies all twelve partitions, which together are the whole chip. Each is read twice and the two reads compared; every error-correction byte is then recomputed on the PC and checked. It stops at any bad block, read error or mismatch. `DIR` ends up with one `mtdN-NAME.dataoob` file per partition (each 2048-byte page followed by its 64 spare bytes), the tool's own summaries, a `MANIFEST` and `SHA256SUMS`, all read-only. About 138 MB.

**The spare bytes matter.** Yealink's filesystem keeps its bookkeeping there, so a plain copy of the data alone cannot be put back into a working Yealink system.

Then:

```
tools/nandbackup.sh check DIR       # no phone: checksums and error-correction bytes
tools/nandrestore.sh check DIR      # no phone: a restore would write back exactly these bytes
tools/nandbackup.sh verify DIR      # the phone again: does it still hold exactly the backup?
```

and copy `DIR` to a second machine. Under Yealink's firmware the `cfg` and `data` partitions change while they are read; take the backup from our kernel for a steady copy. They are Yealink's settings and we never write them.

## What we write, and the way back

Today our firmware writes one place only: the last 60 blocks (7.5 MB) of Yealink's `app` partition, which Yealink's filesystem has never used. Yealink's firmware keeps working beside it. The driver cannot write anywhere else ([flash.md](flash.md)).

Going back to stock is erasing those blocks:

```
tools/nandrestore.sh t48data DIR
```

On the phone, under our kernel, that stops using the flash (`/data` carries on in RAM), erases the 60 blocks, checks that each reads back erased, and then reads the **whole flash** and compares it with the backup. "the phone still holds exactly the backup" for every partition means the phone is as it was. (`cfg` and `data` will differ if Yealink's firmware has run since the backup; that is Yealink saving its own settings.)

If power is lost half way: nothing is harmed. Some blocks are erased and some still hold our data; run it again.

The program on the phone is `nandrestore` ([`native/nandrestore.c`](../native/nandrestore.c)). It is written for the general case, a partition with Yealink's data in it: erase a block, write each page with the backup's spare bytes, read it back, compare all 2112 bytes. That is what a restore of `app` or `kernel` will use once we write those.

## Ways back that need none of this

- **Recovery mode** is in U-Boot, which we never write: hold Speakerphone at power-on, then TFTP ([own-kernel.md](own-kernel.md)). It boots our live image from RAM whatever is in the rest of the flash, and from there the steps above work.
- **Yealink's own recovery** with a stock firmware file, through the same recovery mode, rewrites Yealink's partitions. Erase our blocks first if you can: we have not seen what Yealink's updater does when it meets blocks in `app` that hold data it does not know.
- **Safe mode**: a second copy of Yealink's kernel and root filesystem (`smkern`, `smrfs`), which we never write.

## Tom's phone

Backup taken 2026-11-23 from our kernel, before the first write. Twelve partitions, two identical reads each, 208,056 written 512-byte steps with matching error correction, none differing; no bad blocks; no corrected bits. Copies: `dumps/stock-backup/` on the build container and `~/t48s-stock-backup` on studybox.

```
8e0577e94d07d57fce4b6e31ee451658cc4a327dc1e8f79fabfa827cbc00fc67  mtd0-ubl.dataoob
bf50bd4a2458fa68e160f5cc3283fc14191f387b5c441c54c75a6a396f4fa02a  mtd1-uboot.dataoob
b08bcaa779bbb069304b3dcf2b68248aca37a73fa24fa8205eb63f662f594361  mtd2-env.dataoob
b7ea63b021674d342d379666018989b1487dd00ed5ee56d558f198173a46749b  mtd3-userenv.dataoob
a960afdddea6ef7be3e083a0a6fa255d17a5ae6835da5dc120bd50780064deb2  mtd4-logo.dataoob
5a77b73eddbd1d6fd0f2fc76b17d89217d4fac3910fd571dd724ad2a3bff7cdf  mtd5-smkern.dataoob
ee243b651807c7953f64720a14bf4147fdfad87fcdcfdec96aff3a5ad0fa12b8  mtd6-smrfs.dataoob
5a77b73eddbd1d6fd0f2fc76b17d89217d4fac3910fd571dd724ad2a3bff7cdf  mtd7-kernel.dataoob
ee243b651807c7953f64720a14bf4147fdfad87fcdcfdec96aff3a5ad0fa12b8  mtd8-rfs.dataoob
c59191f6710bac1f3acbd80fbe564a5caf9981c2e00d5c733ab4df0b19ea4624  mtd10-app.dataoob
13916e0faff80d4aa5191c347b566c87b1a77342ad8278ed6a901b3c5655eb35  mtd11-cfg.dataoob
056934b14b262723e2342c962663d5d90385cf993c4249dfde8669645f5ff8c6  mtd12-data.dataoob
```

(The safe-mode kernel and root filesystem are byte-for-byte copies of the normal ones, hence the two pairs of equal sums.)
