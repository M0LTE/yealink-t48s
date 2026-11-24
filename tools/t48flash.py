#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""t48flash.py: what goes into the phone's flash at install time, worked out and
checked on a PC (issues #8, #29, #30; docs/install.md).

  t48flash.py checkkernel T48S-kernel.uimg
        check the kernel file as U-Boot's normal boot will treat it
  t48flash.py kernelimage T48S-kernel.uimg OUT.dataoob
        the exact contents of the kernel partition after the install: every
        page with its 64 spare bytes, error-correction bytes included
  t48flash.py nboot KERNEL.dataoob [T48S-kernel.uimg]
        read a kernel partition (a dump, or the file above) the way U-Boot does
        and check what it would start; with a file, it must be that file
  t48flash.py ubi APP.dataoob [NAME=FILE ...]
        read a dump of the app partition as UBI: list the volumes, and check
        that each named volume holds exactly FILE
  t48flash.py layout
        print the flash layout this tool and the driver agree on

U-Boot's normal boot (its environment, dumps/mtd2-env.bin, and its code):

  bootcmd=run bootnand
  bootnand=setenv bootargs ...; setenv autostart yes; nboot 0x02680000 0 0x015A0000

nboot reads the first page at 0x015A0000, wants the legacy image magic there
(else "Bad Magic Number"), takes the size from the header, reads header and
payload to 0x02680000 skipping bad blocks, and because autostart is yes runs
bootm on it: header CRC, data CRC, ARM, then a copy of the payload to the load
address and a jump. That last part is what recovery mode does with T48S.bin
(tools/mkliveimage.py), so the file is the same kind with another device tree:
one that says t48boot=flash and names no ramdisk.

A page as stored: 2048 data bytes, then 64 spare bytes in four groups of 16, one
per 512 data bytes: bytes 9 to 15 of a group are that step's ECC
(tools/nandcheck.py), everything else in the spare area stays ff.
"""
import hashlib
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkliveimage as live      # noqa: E402
import nandcheck                # noqa: E402

PAGE, SPARE, REC, PPB = 2048, 64, 2112, 64
BLOCK = PAGE * PPB
# Yealink's partition table (U-Boot's mtdparts); the same in the kernel driver
PARTS = [("ubl", 0x00000000, 0x00020000), ("uboot", 0x00020000, 0x00100000),
         ("env", 0x00120000, 0x00100000), ("userenv", 0x00220000, 0x00100000),
         ("logo", 0x00320000, 0x00100000), ("smkern", 0x00420000, 0x00280000),
         ("smrfs", 0x006a0000, 0x00f00000), ("kernel", 0x015a0000, 0x00280000),
         ("rfs", 0x01820000, 0x00f00000), ("app", 0x02720000, 0x04ae0000),
         ("cfg", 0x07200000, 0x00400000), ("data", 0x07600000, 0x00a00000)]
OURS = ("kernel", "app")        # the only partitions the installer writes
KERNEL_SIZE = 0x00280000
NBOOT_ADDR = 0x02680000         # where nboot reads the image to
UBOOT_ADDR = 0x03B00000


def check_kernel(data):
    """(ok, [messages]) for a kernel file meant for the kernel partition."""
    ok, msg = live.check_uimage(data)
    if not ok:
        return False, msg
    size, load, entry = struct.unpack_from(">3I", data, 12)
    os_, arch, typ, comp = data[28:32]
    if 64 + size != len(data):
        return False, msg + ["%d bytes follow the image the header describes" % (len(data) - 64 - size)]
    if len(data) > KERNEL_SIZE:
        return False, msg + ["%d bytes: the kernel partition holds %d" % (len(data), KERNEL_SIZE)]
    if os_ != 5 or typ != 2 or comp != 0 or load != entry:
        return False, msg + ["must be a Linux kernel image, uncompressed for U-Boot, entered at its load address"]
    z = data[64:]
    info = live.zimage_info(z)
    if info is None or "dtb" not in info or "image" not in info:
        return False, msg + ["not a 6.18 kernel with a device tree on its end (kernel6/build.sh with FLASH=1)"]
    if info["end"] + len(info["dtb"]) != len(z):
        return False, msg + ["bytes follow the device tree"]
    dt = live.parse_fdt(info["dtb"])
    chosen = dt.get("/chosen", {})
    args = chosen.get("bootargs", b"").rstrip(b"\0").decode("ascii", "replace")
    if "t48boot=flash" not in args.split():
        return False, msg + ["the command line does not say t48boot=flash: \"%s\"" % args]
    if "linux,initrd-start" in chosen or "initrd=" in args or "mem=" in args:
        return False, msg + ["a flash kernel has no ramdisk, and its memory comes from the device tree"]
    mem = [v for v in dt.values() if v.get("device_type") == b"memory\0"]
    if len(mem) != 1 or len(mem[0].get("reg", b"")) != 8:
        return False, msg + ["the device tree must declare one block of RAM"]
    ram, ramsize = struct.unpack(">2I", mem[0]["reg"])
    msg.append("command line \"%s\"" % args)
    # nboot's read, bootm's copy, and the unpacker's own move must not collide
    if not (ram + info["text_offset"] <= load and load + len(z) <= ram + ramsize):
        return False, msg + ["load address 0x%08x is outside the RAM the device tree declares" % load]
    k0 = ram + info["text_offset"]
    k1 = k0 + info["image"] + info["bss"]
    work = 0x1000 + info["malloc"]
    top = max(load + len(z) + work, k1 + len(z) + work + 0x10000)
    if NBOOT_ADDR + len(data) > UBOOT_ADDR:
        return False, msg + ["read to 0x%08x the file would reach U-Boot" % NBOOT_ADDR]
    if top > ram + ramsize:
        return False, msg + ["the unpacker would work above the declared RAM"]
    msg.append("nboot reads %d bytes to 0x%08x; bootm copies the kernel to 0x%08x; unpacked it is "
               "0x%08x to 0x%08x; the unpacker works below 0x%08x" % (len(data), NBOOT_ADDR, load, k0, k1, top))
    msg.append("kernel partition: %d of %d bytes used, %d free" % (len(data), KERNEL_SIZE, KERNEL_SIZE - len(data)))
    return True, msg


def page_record(data):
    """One page as the driver stores it: the data, ff spare bytes, and the ECC."""
    assert len(data) == PAGE
    spare = bytearray(b"\xff" * SPARE)
    for s in range(PAGE // 512):
        spare[s * 16 + 9:s * 16 + 16] = nandcheck.ecc(data[s * 512:(s + 1) * 512])
    return data + bytes(spare)


def kernel_image(data):
    """The whole kernel partition after the install, as 2112-byte page records."""
    out = bytearray()
    padded = data + b"\xff" * (-len(data) % PAGE)
    for i in range(0, len(padded), PAGE):
        page = padded[i:i + PAGE]
        if page == b"\xff" * PAGE:
            # the driver leaves an all-ff page erased, with no ECC; U-Boot would
            # have to treat an erased page inside the image as good data
            raise ValueError("page %d of the kernel is all ff; build it again" % (i // PAGE))
        out += page_record(page)
    out += b"\xff" * (KERNEL_SIZE // PAGE * REC - len(out))
    return bytes(out)


def records(blob):
    if len(blob) % REC:
        raise ValueError("not a whole number of 2112-byte pages")
    return [(blob[i:i + PAGE], blob[i + PAGE:i + REC]) for i in range(0, len(blob), REC)]


def nboot(blob):
    """Read a kernel partition as U-Boot's nboot does. Returns the image bytes.
    Bad blocks (spare byte 0 of a block's first page not ff) are skipped, as
    nand_read_skip_bad does; every page read is ECC-checked here as well."""
    recs = records(blob)
    good = []
    for b in range(0, len(recs), PPB):
        if recs[b][1][0] == 0xff:
            good.extend(recs[b:b + PPB])
    if not good or struct.unpack_from(">I", good[0][0], 0)[0] != live.UIMAGE_MAGIC:
        raise ValueError("Bad Magic Number: U-Boot would not start this")
    size = 64 + struct.unpack_from(">I", good[0][0], 12)[0]
    need = -(-size // PAGE)
    if need > len(good):
        raise ValueError("the header asks for more than the partition holds")
    out = bytearray()
    for n in range(need):
        data, spare = good[n]
        for s in range(4):
            if nandcheck.ecc(data[s * 512:(s + 1) * 512]) != spare[s * 16 + 9:s * 16 + 16]:
                raise ValueError("page %d step %d: ECC does not match the data" % (n, s))
        out += data
    return bytes(out[:size])


# ---- UBI, read from a dump. On the phone there are no sub-page writes, so the two
# headers are in pages 0 and 1 and the data starts at page 2; each block's first
# header says where, and that is what is followed here. ----
UBI_LAYOUT_VOL = 0x7FFFEFFF


def ubi_read(blob, plain=False):
    """{"volumes": {name: {...}}, "pebs": counts}. Data comes back per volume."""
    rec = PAGE if plain else REC
    if len(blob) % (rec * PPB):
        raise ValueError("not a whole number of blocks")
    nblocks = len(blob) // (rec * PPB)

    def page(b, p):
        o = (b * PPB + p) * rec
        return blob[o:o + PAGE]

    def at(b, off, n):
        """n bytes of block b's data from byte off"""
        out = b"".join(page(b, p) for p in range(off // PAGE, -(-(off + n) // PAGE)))
        return out[off % PAGE:off % PAGE + n]

    lebs, counts, data_offset = {}, {"ubi": 0, "free": 0, "erased": 0, "other": 0}, [None]
    for b in range(nblocks):
        ec = page(b, 0)
        if ec[:4] != b"UBI#":
            counts["erased" if ec == b"\xff" * PAGE else "other"] += 1
            continue
        if (~zlib.crc32(ec[:60]) & 0xFFFFFFFF) != struct.unpack_from(">I", ec, 60)[0]:
            counts["other"] += 1
            continue
        counts["ubi"] += 1
        vid_off, data_off = struct.unpack_from(">2I", ec, 16)
        if data_offset[0] is None:
            data_offset[0] = data_off
        vid = at(b, vid_off, 64)
        if vid[:4] != b"UBI!":
            counts["free"] += 1
            continue
        if (~zlib.crc32(vid[:60]) & 0xFFFFFFFF) != struct.unpack_from(">I", vid, 60)[0]:
            raise ValueError("block %d: damaged volume header" % b)
        vol_type = vid[5]
        vol_id, lnum = struct.unpack_from(">2I", vid, 8)
        data_size, used_ebs, data_pad, data_crc = struct.unpack_from(">4I", vid, 20)
        sqnum, = struct.unpack_from(">Q", vid, 40)
        cur = lebs.get((vol_id, lnum))
        if cur is None or cur[0] < sqnum:
            lebs[(vol_id, lnum)] = (sqnum, b, vol_type, data_size, used_ebs, data_crc)
    if data_offset[0] is None:
        raise ValueError("no UBI here")
    leb_size = BLOCK - data_offset[0]

    def leb(b, n=leb_size):
        return at(b, data_offset[0], n)

    # the volume table: two copies, 128 records of 172 bytes
    table = None
    for copy in (0, 1):
        if (UBI_LAYOUT_VOL, copy) in lebs:
            table = leb(lebs[(UBI_LAYOUT_VOL, copy)][1])
            break
    if table is None:
        raise ValueError("no UBI volume table")
    vols = {}
    for vid_ in range(128):
        r = table[vid_ * 172:(vid_ + 1) * 172]
        reserved, align, pad = struct.unpack_from(">3I", r, 0)
        if not reserved:
            continue
        if (~zlib.crc32(r[:168]) & 0xFFFFFFFF) != struct.unpack_from(">I", r, 168)[0]:
            raise ValueError("volume table record %d is damaged" % vid_)
        vtype = r[12]
        nlen, = struct.unpack_from(">H", r, 14)
        name = r[16:16 + nlen].decode()
        mine = {l: v for (v_, l), v in lebs.items() if v_ == vid_}
        data = None
        if vtype == 2:                                  # static: exact length, CRC per block
            parts = []
            for l in sorted(mine):
                sq, b, t, dsize, used, crc = mine[l]
                d = leb(b, dsize)
                if (~zlib.crc32(d) & 0xFFFFFFFF) != crc:
                    raise ValueError("volume %s block %d: data CRC" % (name, l))
                parts.append(d)
            used = mine[min(mine)][4] if mine else 0
            if len(mine) != used:
                raise ValueError("volume %s: %d of %d blocks present" % (name, len(mine), used))
            data = b"".join(parts)
        vols[name] = {"id": vid_, "type": "static" if vtype == 2 else "dynamic", "reserved": reserved,
                      "mapped": len(mine), "data": data}
    return {"volumes": vols, "pebs": counts, "blocks": nblocks}


def main(argv):
    cmd = argv[1] if len(argv) > 1 else ""
    if cmd == "layout":
        for name, off, size in PARTS:
            print("%-8s 0x%08x to 0x%08x  %4d blocks  %s" % (name, off, off + size, size // BLOCK,
                                                           "OURS after install" if name in OURS else "never written"))
        return 0
    if cmd == "checkkernel" and len(argv) == 3:
        ok, msg = check_kernel(open(argv[2], "rb").read())
        for m in msg[:-1] if not ok else msg:
            print("  " + m)
        if not ok:
            print("FAIL: " + msg[-1])
        print("PASS" if ok else "REJECTED")
        return 0 if ok else 1
    if cmd == "kernelimage" and len(argv) == 4:
        data = open(argv[2], "rb").read()
        ok, msg = check_kernel(data)
        if not ok:
            print("REJECTED: " + msg[-1], file=sys.stderr)
            return 1
        img = kernel_image(data)
        if nboot(img) != data:
            print("the image does not read back as the kernel file", file=sys.stderr)
            return 1
        open(argv[3], "wb").write(img)
        written = sum(1 for d, s in records(img) if d != b"\xff" * PAGE)
        print("%s: %d pages to write, %d left erased, sha256 %s" % (
            argv[3], written, len(img) // REC - written, hashlib.sha256(img).hexdigest()))
        return 0
    if cmd == "nboot" and len(argv) in (3, 4):
        try:
            img = nboot(open(argv[2], "rb").read())
        except ValueError as e:
            print("REJECTED: %s" % e)
            return 1
        ok, msg = live.check_uimage(img)
        for m in msg:
            print("  " + m)
        if not ok:
            print("REJECTED")
            return 1
        if len(argv) == 4:
            if img != open(argv[3], "rb").read():
                print("REJECTED: U-Boot would read something other than %s" % argv[3])
                return 1
            print("  byte for byte %s" % argv[3])
        print("PASS: U-Boot reads %d bytes from the partition, both checksums right" % len(img))
        return 0
    if cmd == "ubi" and len(argv) >= 3:
        opts = dict(a.split("=", 1) for a in argv[3:] if "=" in a)
        blob = open(argv[2], "rb").read()
        try:
            u = ubi_read(blob, plain="plain" in argv[3:])
        except ValueError as e:
            print("REJECTED: %s" % e)
            return 1
        c = u["pebs"]
        print("%d blocks: %d UBI (%d of them free), %d erased, %d other" % (u["blocks"], c["ubi"], c["free"], c["erased"], c["other"]))
        rc = 0
        for name, v in sorted(u["volumes"].items(), key=lambda kv: kv[1]["id"]):
            line = "  %-12s %-7s %4d blocks reserved, %4d in use" % (name, v["type"], v["reserved"], v["mapped"])
            if v["data"] is not None:
                line += ", %d bytes, sha256 %s" % (len(v["data"]), hashlib.sha256(v["data"]).hexdigest()[:16])
            print(line)
        for name, path in opts.items():
            v = u["volumes"].get(name)
            want = open(path, "rb").read()
            if v is None or v["data"] != want:
                print("REJECTED: volume %s does not hold exactly %s" % (name, path))
                rc = 1
            else:
                print("  %s holds exactly %s" % (name, path))
        if c["other"]:
            print("REJECTED: %d blocks hold something that is neither UBI nor erased" % c["other"])
            rc = 1
        print("PASS" if not rc else "FAILED")
        return rc
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
