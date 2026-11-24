#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mkliveimage.py: build (and check) the "try it without installing" live image.

Issue #7, docs/live-image.md. U-Boot's recovery mode (hold Speakerphone at
power-on, then 2 for a USB stick or 1 for TFTP) reads three files named after the
board (T48S.rom, T48S.bin, T48S.rfs), checks only the 128-byte head of the .rom,
and then boots the .bin with the .rfs as its ramdisk. It writes nothing to the
phone's flash except one flag in its own settings (BOOTMODE, see the doc).

So a shareable live image is: our kernel as T48S.bin, our system image inside
T48S.rfs, and a SMALL CRAFTED T48S.rom whose header passes U-Boot's check
WITHOUT being a copy of Yealink's firmware (which we may not redistribute).

  mkliveimage.py usb DIR bin=T48S.bin system=system.img
        the files for a USB stick, ready to copy: ROM, kernel, ramdisk and a
        README.txt for the owner (rfs=FILE instead of system= takes a ready ramdisk)
  mkliveimage.py checkstick STICK [DIR]
        read a stick (/dev/sdX, as root) or an image of one the way U-Boot will:
        partition table, FAT, the three files; DIR compares them with a built set
  mkliveimage.py build OUTDIR bin=T48S.bin system=system.img
        the same three files for a TFTP server, without the README
  mkliveimage.py rfs IN OUT      make T48S.rfs from Buildroot's system.img
  mkliveimage.py check ROM       emulate U-Boot's check of a .rom file
  mkliveimage.py checkbin T48S.bin [kernel=3.6|6.18]
        check a kernel file as U-Boot and the kernel's unpacker will treat it

Either kernel can be the .bin: Linux 3.6 (kernel/build.sh live, the default) or
Linux 6.18 (kernel6/build.sh with LIVE=1; say kernel=6.18). The ROM and the
ramdisk are the same for both. What is checked in a 6.18 file is under
check_kernel6 below.

The ramdisk. U-Boot loads the kernel at 0x02680000 and the ramdisk 4 MB above it
at 0x02a80000, so the kernel file must be under 4 MB. Our kernel takes the 8 MB
from 0x02a80000 as its initrd whatever the file's size (kernel/live.config), so
T48S.rfs is a cpio archive followed by zeros up to exactly 8 MB: the kernel
unpacks the archive into RAM and skips the zeros. Without the padding it would
read whatever was in memory after the file and give up.

What is in the archive: one file, system.img, the read-only system image
(squashfs) that Buildroot makes. The live kernel has the first stage built in
(kernel/build.sh live); the archive is unpacked over it, so the first stage finds
/system.img in RAM beside it exactly as on a kexec boot (docs/boot.md). The
archive is not compressed: the system image already is.

What U-Boot actually checks (reverse-engineered from the T48S U-Boot at
0x03B15274, confirmed byte for byte against a genuine 66.82.0.31 ROM):

  off  sz  field                 check
  0x00  4  magic (BE u32)        == 0xAD24EC0B            else "Rom magic Err"
  0x04  4  header length (BE)    the number of bytes the header CRC covers (0x80)
  0x08  4  header CRC32 (BE)     crc32(header[0:len] with this field zeroed),
                                 seed 0, IEEE/zlib                else "Rom crc Err"
  0x0c  4  tool/format           not checked against the board
  0x10  4  flags                 bit0 "force" -> skip hwid/swid checks;
                                 bit16 "skipoem" -> skip the OEM check
  0x14  4  body length           informational (U-Boot does not read the body)
  0x18  4  0xffffffff            not checked
  0x1c  4  module count          not checked (U-Boot runs no module loop)
  0x20  4  primary hwid (BE)     compared to the board's hwid byte; if it does
                                 not match, U-Boot scans the 16-byte list at
                                 0x64 for the board's hwid      else "Rom type Err"
  0x24 16  model name            not checked
  0x34 16  OEM name              strcmp vs the phone's "phoneoem" env, unless
                                 skipoem                         else "Rom OEM Err"
  0x44 24  version string        not checked
  0x5c  4  hw revision (BE)      >= the phone's hw rev, unless force "Rom hwid %d<%d"
  0x60  4  swid (BE)             >= the phone's "phonesid" env, unless force "Rom swid"
  0x64 16  supported-hwid list   bytes; the board's hwid must appear here (or at
                                 0x20). Yealink's own T46S ROM lists 65,67,68
                                 ("ACD"), which is why it already boots on a T48S.

There is NO cryptographic signature anywhere. The whole gate is a CRC plus some
model/version fields. The crafted ROM here sets the "force" and "skipoem" flags
so hwid/swid/OEM are not compared (those differ between firmware versions and
resellers), and lists every T4xS hwid at 0x20/0x64 so the type check passes on
any of them. The body is zero padding: U-Boot never looks at it, and we boot our
own .bin, not the ROM.

Hardware ids (from Yealink's rcS): 65=T48S 66=T46S 67=T42S 68=T41S 69=T27G
70=T54S 74=T52S.
"""
import gzip
import hashlib
import io
import os
import struct
import sys
import zlib

MAGIC = 0xAD24EC0B
HDR_LEN = 0x80                 # header CRC covers exactly this many bytes
PAD_TO = 4096                  # U-Boot pulls ~2936 bytes of head before it stops
FLAG_FORCE = 0x00000001        # skip the hwid / swid "not lower" checks
FLAG_SKIPOEM = 0x00010000      # skip the OEM strcmp

# every T4xS this U-Boot family serves; the board's own id must be in this set
T4XS_HWIDS = (65, 66, 67, 68, 69, 70, 74)
MODEL_HWID = {"T48S": 65, "T46S": 66, "T42S": 67, "T41S": 68,
              "T27G": 69, "T54S": 70, "T52S": 74}


def _str_field(s, n):
    b = s.encode("ascii")[:n - 1]
    return b + b"\0" * (n - len(b))


def craft_rom(model="T48S"):
    """Return the bytes of a crafted T48S.rom that passes U-Boot's check."""
    h = bytearray(HDR_LEN)
    struct.pack_into(">I", h, 0x00, MAGIC)
    struct.pack_into(">I", h, 0x04, HDR_LEN)
    struct.pack_into(">I", h, 0x08, 0)              # CRC filled in below
    struct.pack_into(">I", h, 0x0c, 0x03000003)     # tool/format, copied from genuine
    struct.pack_into(">I", h, 0x10, FLAG_FORCE | FLAG_SKIPOEM)
    struct.pack_into(">I", h, 0x14, PAD_TO)         # body length (informational)
    struct.pack_into(">I", h, 0x18, 0xFFFFFFFF)
    struct.pack_into(">I", h, 0x1c, 0)              # no modules
    struct.pack_into(">I", h, 0x20, MODEL_HWID.get(model, 0))
    h[0x24:0x34] = _str_field(model, 16)
    h[0x34:0x44] = _str_field("NULL", 16)           # OEM (skipped by skipoem anyway)
    h[0x44:0x5c] = _str_field("99.99.99.99", 24)    # version (skipped by force)
    struct.pack_into(">I", h, 0x5c, 0)              # hw rev (skipped by force)
    struct.pack_into(">I", h, 0x60, 0)              # swid (skipped by force)
    for i, hwid in enumerate(T4XS_HWIDS):           # supported-hwid list at 0x64
        h[0x64 + i] = hwid
    crc = zlib.crc32(bytes(h)) & 0xFFFFFFFF          # field at 0x08 is already zero
    struct.pack_into(">I", h, 0x08, crc)
    return bytes(h) + b"\0" * (PAD_TO - HDR_LEN)


def check_rom(data, board_hwid=65, board_hwrev=0, phonesid=0, phoneoem="NULL"):
    """Emulate the T48S U-Boot romcheck (0x03B15274). Return (ok, [messages]).

    Mirrors the exact order and tests U-Boot uses, so a crafted ROM can be
    validated offline. board_hwid is the phone's hwid (65 for a T48S)."""
    msg = []
    if len(data) < HDR_LEN:
        return False, ["shorter than a 128-byte header"]
    hlen = struct.unpack_from(">I", data, 0x04)[0]
    if hlen < HDR_LEN or hlen > len(data):
        return False, ["header length 0x%x out of range" % hlen]
    # CRC: over header[0:hlen] with the 4 bytes at 0x08 zeroed
    h = bytearray(data[:hlen])
    stored_crc = struct.unpack_from(">I", h, 0x08)[0]
    struct.pack_into(">I", h, 0x08, 0)
    calc = zlib.crc32(bytes(h)) & 0xFFFFFFFF
    if calc != stored_crc:
        return False, ["Rom crc Err (stored 0x%08x, computed 0x%08x)" % (stored_crc, calc)]
    msg.append("crc ok (0x%08x over %d bytes)" % (calc, hlen))
    if struct.unpack_from(">I", data, 0x00)[0] != MAGIC:
        return False, msg + ["Rom magic Err"]
    msg.append("magic ok")
    # type: 0x20 or the 0x64 list must contain the board hwid
    primary = struct.unpack_from(">I", data, 0x20)[0]
    supported = set(data[0x64:0x74]) - {0}
    if primary != board_hwid and board_hwid not in supported:
        return False, msg + ["Rom type Err %d not in {%d} u %s" %
                             (board_hwid, primary, sorted(supported))]
    msg.append("type ok (board hwid %d accepted)" % board_hwid)
    flags = struct.unpack_from(">I", data, 0x10)[0]
    if flags & FLAG_FORCE:
        msg.append("force flag set: hwid/swid checks skipped")
    else:
        if struct.unpack_from(">I", data, 0x5c)[0] < board_hwrev:
            return False, msg + ["Rom hwid too low"]
        if struct.unpack_from(">I", data, 0x60)[0] < phonesid:
            return False, msg + ["Rom swid too low"]
        msg.append("hwid/swid ok")
    if flags & FLAG_SKIPOEM:
        msg.append("skipoem flag set: OEM check skipped")
    else:
        oem = data[0x34:0x44].split(b"\0")[0].decode("ascii", "replace")
        if oem != phoneoem:
            return False, msg + ["Rom OEM Err %r != %r" % (oem, phoneoem)]
        msg.append("oem ok")
    return True, msg + ["Rom check ok"]


KERNEL_MAX = 0x400000          # the ramdisk is loaded 4 MB above the kernel file
RFS_SIZE = 0x800000            # what the kernel takes as its initrd (initrd=...,0x800000)
UIMAGE_MAGIC = 0x27051956


def check_uimage(data):
    """U-Boot's bootm checks on a legacy image: magic, both CRCs, ARM. (ok, [messages])"""
    if len(data) < 64 or struct.unpack_from(">I", data, 0)[0] != UIMAGE_MAGIC:
        return False, ["not a U-Boot legacy image (mkimage output): Bad Magic Number"]
    hcrc, _, size, load, entry, dcrc = struct.unpack_from(">6I", data, 4)
    os_, arch, typ, comp = data[28:32]
    name = data[32:64].split(b"\0")[0].decode("ascii", "replace")
    if zlib.crc32(data[:4] + b"\0\0\0\0" + data[8:64]) & 0xFFFFFFFF != hcrc:
        return False, ["Bad Header Checksum"]
    if len(data) < 64 + size or zlib.crc32(data[64:64 + size]) & 0xFFFFFFFF != dcrc:
        return False, ["Bad Data CRC (truncated or damaged file)"]
    if arch != 2:
        return False, ["Unsupported Architecture 0x%x (want 2, ARM)" % arch]
    msg = ["kernel image ok: \"%s\", %d bytes, load 0x%08x" % (name, size, load)]
    if len(data) >= KERNEL_MAX:
        return False, msg + ["%d bytes: must be under 4 MB, U-Boot loads the ramdisk "
                             "4 MB above it and would overwrite its end" % len(data)]
    return True, msg


# ---- which kernel a .bin holds, and the addresses of a 6.18 one ----
# U-Boot (bootm, from the disassembly at 0x03B20448 and the serial logs) reads the
# file to 0x02680000, copies the payload to the header's load address, and enters
# it with r0 = 0, r1 = 2342, r2 = a tag list: ATAG_CORE, one ATAG_MEM, ATAG_CMDLINE
# (Yealink's bootargs), ATAG_NONE. No ramdisk tag and no device tree.
#
# Linux 3.6 has its memory, command line and ramdisk address compiled in
# (kernel/live.config). Linux 6.18 takes all three from the device tree appended
# to the zImage (kernel6/dts/t48s-live.dtsi) and never reads the tags. So for 6.18
# the things that must agree are all inside the one file, and are checked here.
BIN_ADDR = 0x02680000          # where U-Boot reads the .bin
RFS_ADDR = 0x02a80000          # where U-Boot reads the .rfs
UBOOT_ADDR = 0x03B00000        # U-Boot itself
DEFAULT_KERNEL = "3.6"         # until issue 55 makes 6.18 the phone's kernel
KERNELS = ("3.6", "6.18")
ZIMAGE_MAGIC = 0x016F2818
FDT_MAGIC = 0xD00DFEED


def parse_fdt(blob):
    """A flattened device tree as {"/path": {property: bytes}}."""
    magic, total, off_struct, off_strings = struct.unpack_from(">4I", blob, 0)
    if magic != FDT_MAGIC or total > len(blob):
        raise ValueError("not a device tree")
    nodes, path, p = {}, [], off_struct
    while True:
        tok = struct.unpack_from(">I", blob, p)[0]
        p += 4
        if tok == 1:                                    # begin node
            end = blob.index(b"\0", p)
            path.append(blob[p:end].decode())
            p = (end + 4) & ~3
            nodes["/" + "/".join(path[1:])] = {}
        elif tok == 2:                                  # end node
            path.pop()
        elif tok == 3:                                  # property
            ln, nameoff = struct.unpack_from(">2I", blob, p)
            s = off_strings + nameoff
            name = blob[s:blob.index(b"\0", s)].decode()
            nodes["/" + "/".join(path[1:])][name] = blob[p + 8:p + 8 + ln]
            p = (p + 8 + ln + 3) & ~3
        elif tok == 9:                                  # end
            return nodes
        elif tok != 4:                                  # nop
            raise ValueError("damaged device tree")


def zimage_info(z):
    """What an ARM zImage says about itself, or None if it is not one. The device
    tree, if one is appended, starts at "end"."""
    if len(z) < 0x40 or struct.unpack_from("<I", z, 0x24)[0] != ZIMAGE_MAGIC:
        return None
    start, end = struct.unpack_from("<2I", z, 0x28)
    info = {"end": end - start}
    if struct.unpack_from("<I", z, 0x34)[0] == 0x45454545:      # the table newer kernels carry
        t = struct.unpack_from("<I", z, 0x38)[0]
        while t + 8 <= len(z):
            words, tag = struct.unpack_from("<2I", z, t)
            if words == 0:
                break
            if tag == 0x5A534C4B and words >= 6:
                size_at, bss, text_offset, malloc = struct.unpack_from("<4I", z, t + 8)
                info.update(image=struct.unpack_from("<I", z, size_at)[0], bss=bss,
                            text_offset=text_offset, malloc=malloc)
            t += 4 * words
    dtb = z[info["end"]:]
    if len(dtb) >= 8 and struct.unpack_from(">I", dtb, 0)[0] == FDT_MAGIC:
        info["dtb"] = dtb[:struct.unpack_from(">I", dtb, 4)[0]]
    return info


def bin_kernel(data):
    """Which kernel a .bin (already through check_uimage) holds: "6.18" (a device
    tree on its end), "3.6" (ours, by its name), or "other" (Yealink's own, as in
    the first stick test). None if it is not a Linux zImage."""
    info = zimage_info(data[64:])
    if info is None:
        return None
    if "dtb" in info:
        return "6.18"
    return "3.6" if data[32:64].startswith(b"Linux-3.6.") else "other"


def check_kernel6(data):
    """A Linux 6.18 T48S.bin, from the file alone: will U-Boot's handover and the
    kernel's unpacker put everything where the kernel then looks, with nothing on
    top of anything else? (ok, [messages])

    - the payload is a zImage with a device tree on its end, and nothing after it;
    - the device tree names the ramdisk as exactly the 8 MB U-Boot loads T48S.rfs
      into, inside the RAM it declares and clear of the frame buffers, and its
      command line says t48boot=live;
    - U-Boot's copy of the kernel to the load address lands in declared RAM (so the
      kernel can keep hold of its device tree), below the ramdisk and U-Boot;
    - the unpacked kernel with its zeroed data, and above it the unpacker with the
      device tree, its stack and its heap after it has moved itself out of the
      way, all end below the ramdisk.
    """
    load, entry = struct.unpack_from(">2I", data, 16)
    comp = data[31]
    z = data[64:64 + struct.unpack_from(">I", data, 12)[0]]
    info = zimage_info(z)
    msg = []
    if info is None or "dtb" not in info or "image" not in info:
        return False, ["not a 6.18 kernel with a device tree on its end (kernel6/build.sh with LIVE=1)"]
    if comp != 0 or load != entry:
        return False, ["the image must be uncompressed for U-Boot, entered at its load address"]
    if info["end"] + len(info["dtb"]) != len(z):
        return False, ["%d bytes follow the device tree" % (len(z) - info["end"] - len(info["dtb"]))]
    try:
        dt = parse_fdt(info["dtb"])
    except (ValueError, struct.error, IndexError):
        return False, ["the device tree on the kernel's end is damaged"]
    chosen = dt.get("/chosen", {})
    mem = [v for k, v in dt.items() if v.get("device_type") == b"memory\0"]
    if len(mem) != 1 or len(mem[0].get("reg", b"")) != 8:
        return False, ["the device tree must declare one block of RAM"]
    ram, ramsize = struct.unpack(">2I", mem[0]["reg"])
    ramend = ram + ramsize
    args = chosen.get("bootargs", b"").rstrip(b"\0").decode("ascii", "replace")
    if "t48boot=live" not in args.split():
        return False, ["the command line does not say t48boot=live: \"%s\"" % args]
    if "initrd=" in args or "mem=" in args:
        return False, ["the command line must not name memory or a ramdisk: the device tree does"]
    try:
        rd0, = struct.unpack(">I", chosen["linux,initrd-start"])
        rd1, = struct.unpack(">I", chosen["linux,initrd-end"])
    except (KeyError, struct.error):
        return False, ["the device tree names no ramdisk (linux,initrd-start and -end)"]
    if rd0 != RFS_ADDR or rd1 != RFS_ADDR + RFS_SIZE:
        return False, ["ramdisk at 0x%08x to 0x%08x: U-Boot loads the 8 MB of T48S.rfs at 0x%08x" % (rd0, rd1, RFS_ADDR)]
    if not (ram <= rd0 and rd1 <= ramend):
        return False, ["the ramdisk is outside the RAM the device tree declares"]
    msg.append("ramdisk 0x%08x to 0x%08x, as U-Boot loads it; command line \"%s\"" % (rd0, rd1, args))
    for name, node in dt.items():
        if name.startswith("/reserved-memory/") and len(node.get("reg", b"")) == 8:
            a, n = struct.unpack(">2I", node["reg"])
            if a < rd1 and rd0 < a + n:
                return False, ["the ramdisk overlaps reserved memory %s" % name]
    # U-Boot's copy to the load address
    zend = load + len(z)
    if not (ram + info["text_offset"] <= load and zend <= ramend):
        return False, ["load address 0x%08x is not in the RAM the device tree declares (0x%08x to 0x%08x): "
                       "the kernel would lose its device tree" % (load, ram, ramend)]
    work = 0x1000 + info["malloc"]                      # the unpacker's stack and heap, after the device tree
    if zend + work > RFS_ADDR:
        return False, ["loaded at 0x%08x the kernel file reaches 0x%08x: into the ramdisk" % (load, zend + work)]
    # the kernel proper, and the unpacker once it has moved clear of it
    k0 = ram + info["text_offset"]
    k1 = k0 + info["image"] + info["bss"]
    top = max(zend + work, k1 + len(z) + work + 0x10000) if load < k1 else zend + work
    msg.append("kernel 0x%08x to 0x%08x (%d bytes unpacked, %d zeroed); entered at 0x%08x; "
               "the unpacker works below 0x%08x" % (k0, k1, info["image"], info["bss"], load, top))
    if k1 > RFS_ADDR or top > RFS_ADDR:
        return False, msg + ["the unpacked kernel or its unpacker would reach the ramdisk at 0x%08x" % RFS_ADDR]
    msg.append("%.1f MB clear below the ramdisk at 0x%08x; device tree %d bytes on the kernel's end"
               % ((RFS_ADDR - top) / 1048576.0, RFS_ADDR, len(info["dtb"])))
    return True, msg


def check_bin(data, want=None):
    """A .bin in full: U-Boot's checks, which kernel it is, and for 6.18 the
    addresses. want names the kernel it must be. (ok, [messages], kernel)"""
    ok, msg = check_uimage(data)
    if not ok:
        return False, msg, None
    kern = bin_kernel(data)
    if kern is None:
        return False, msg + ["the image does not hold a Linux zImage"], None
    msg.append("Linux %s" % kern if kern != "other" else "not a kernel of ours")
    # a 6.18 file is only taken when asked for, and asking for one takes nothing else
    if want and want != kern and "6.18" in (want, kern):
        return False, msg + ["this is %s, and kernel=%s was asked for (the default is %s)"
                             % ("a Linux %s kernel" % kern if kern != "other" else "not a kernel of ours",
                                want, DEFAULT_KERNEL)], kern
    if kern == "6.18":
        ok, more = check_kernel6(data)
        return ok, msg + more, kern
    return True, msg, kern


def _cpio_entry(ino, name, mode, body=b""):
    n = name.encode() + b"\0"
    hdr = "070701" + "".join("%08X" % v for v in
                             (ino, mode, 0, 0, 1, 0, len(body), 0, 0, 0, 0, len(n), 0))
    rec = hdr.encode() + n
    rec += b"\0" * (-len(rec) % 4)
    return rec + body + b"\0" * (-len(body) % 4)


def make_rfs(data):
    """T48S.rfs from Buildroot's system.img (squashfs): a cpio archive holding it as
    system.img, then zeros to 8 MB. A cpio archive (plain or gzip) is taken as the
    archive itself; an ext2 image or a ready 8 MB ramdisk goes through unchanged.
    (bytes, description)"""
    if len(data) == RFS_SIZE and (data[:2] == b"\x1f\x8b" or data[:6] == b"070701"):
        return data, "ready-made ramdisk"
    if data[:4] == b"hsqs":
        raw = len(data)
        data = _cpio_entry(1, "system.img", 0o100444, data) + _cpio_entry(0, "TRAILER!!!", 0)
        if len(data) > RFS_SIZE:
            raise ValueError("system image is %d bytes: with its wrapping the limit is 8 MB" % raw)
        return data + b"\0" * (RFS_SIZE - len(data)), "system image, %d bytes, as system.img in a cpio archive, zeros to 8 MB" % raw
    if len(data) > 1082 and data[1080:1082] == b"\x53\xef":
        if len(data) > RFS_SIZE:
            raise ValueError("ext2 ramdisk is %d bytes: the limit is 8 MB" % len(data))
        return data, "ext2 image, %d bytes, unchanged" % len(data)
    if data[:6] in (b"070701", b"070702"):
        raw = len(data)
        buf = io.BytesIO()
        with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as g:
            g.write(data)
        data = buf.getvalue()
        what = "cpio, %d bytes, gzip to %d" % (raw, len(data))
    elif data[:2] == b"\x1f\x8b":
        if gzip.decompress(data)[:6] not in (b"070701", b"070702"):
            raise ValueError("gzip file does not hold a cpio archive")
        what = "gzip cpio, %d bytes" % len(data)
    else:
        raise ValueError("not a system image (squashfs), a cpio archive or an ext2 image")
    if len(data) > RFS_SIZE:
        raise ValueError("root filesystem is %d bytes compressed: the limit is 8 MB" % len(data))
    return data + b"\0" * (RFS_SIZE - len(data)), what + ", zeros to 8 MB"


README = """\
Open firmware for the Yealink SIP-{model}: try it from this USB stick
===================================================================

This stick starts the open firmware on your phone WITHOUT installing it.
It runs from memory. Your Yealink firmware and its settings stay in the
phone, and a normal power-up brings them back.

Project and full guide: https://github.com/M0LTE/yealink-t48s
(docs/guide.md)

To try it
---------
1. Unplug the phone's power (or its network cable, if it is powered
   over the network cable).
2. Put this stick in the phone's USB socket.
3. Hold the SPEAKERPHONE key down and plug the power back in. Keep
   holding for about 5 seconds, until the screen shows a menu:
       1. TFTP    2. USB
   Let go. Do NOT press or hold OK at this menu: OK there resets the
   phone's settings.
4. Press 2. The screen says "Start Updating...", and within a minute
   or so the open firmware's clock appears.

Settings (network, SIP account) are entered on the phone's screen. They
are kept in memory only, so they are gone when you unplug it.

To install it for good
----------------------
On the phone: Settings, Install. Back up first (it copies the phone's
whole flash onto this stick and checks the copy; about 10 minutes, and
the stick needs 160 MB free), then Install. Keep the t48s-backup
folder: it is the way back to Yealink's firmware, from the same page.

Power: use a 5 V supply of 1.5 A or more. Yealink's 0.6 A adapter is
too small to run the phone and a stick: the phone freezes.

To go back to Yealink
---------------------
1. Unplug the power.
2. TAKE THIS STICK OUT. This matters: if the stick is still in the
   phone at the next power-up, Yealink's firmware tries to update itself
   from it, fails, and resets your phone's settings on the way.
3. Plug the power back in and do not hold any key. If the screen stays
   on the Yealink logo for more than two minutes, unplug and plug in
   once more.

If it does not start
--------------------
- The menu never appears: you let go too early, or held another key.
- It stops at "Start Updating...": most often the power supply (see
  above). Otherwise:
- "Usb failed" on the screen: the phone could not read this stick. Unplug the power, take the stick out,
  power up normally. Then try another stick: a plain USB 2.0 one of
  32 GB or less (not USB 3, not a card reader), set up with ONE
  partition, old-style "MBR" partition table, formatted FAT32. The
  guide above has the exact steps for Windows, macOS and Linux.
- "Update failed": the files are damaged; copy them again.

Nothing here can damage the phone: the part of it that reads the stick
is never changed.

What is on this stick
---------------------
{files}
The .rom file is not firmware. It is a 4 KB label the phone checks
before it will start the other two files.

Licences: the kernel is GPL-2.0, the rest of the firmware is
AGPL-3.0-or-later, and it includes other free software under its own
licences. Source code: the project page above.
"""


def write_set(outdir, bin_path, rfs_path, system_path, model="T48S", readme=False, kern=DEFAULT_KERNEL):
    """Write MODEL.rom, .bin and .rfs (and README.txt) into outdir. Returns 0 or 2."""
    os.makedirs(outdir, exist_ok=True)
    rom = craft_rom(model)
    ok, why = check_rom(rom, board_hwid=MODEL_HWID.get(model, 65))
    if not ok:
        print("refusing to write a ROM our own check rejects:", why[-1], file=sys.stderr)
        return 2
    if not bin_path or not (rfs_path or system_path):
        print("give bin=KERNEL (kernel/build.sh live, or kernel6/build.sh with LIVE=1 and "
              "kernel=6.18 here) and system=SYSTEM.IMG (buildroot/build.sh) or rfs=RAMDISK", file=sys.stderr)
        return 2
    kernel = open(bin_path, "rb").read()
    ok, kwhy, _ = check_bin(kernel, kern)
    if not ok:
        print("%s: %s" % (bin_path, kwhy[-1]), file=sys.stderr)
        return 2
    for m in kwhy[1:]:
        print("  " + m)
    try:
        rfs, rwhat = make_rfs(open(system_path or rfs_path, "rb").read())
    except ValueError as e:
        print("%s: %s" % (system_path or rfs_path, e), file=sys.stderr)
        return 2
    files = ((model + ".rom", rom, "the label U-Boot checks: " + why[-1]),
             (model + ".bin", kernel, kwhy[0]),
             (model + ".rfs", rfs, "ramdisk: " + rwhat))
    listing = ""
    for name, data, what in files:
        with open(os.path.join(outdir, name), "wb") as f:
            f.write(data)
        print("wrote %s (%d bytes): %s" % (os.path.join(outdir, name), len(data), what))
        listing += "%-9s %8d bytes  sha256 %s\n" % (name, len(data), hashlib.sha256(data).hexdigest())
    if readme:
        text = README.format(model=model, files=listing)
        text.encode("ascii")                 # plain ASCII or this raises
        with open(os.path.join(outdir, "README.txt"), "w", newline="\r\n") as f:
            f.write(text)
        print("wrote %s" % os.path.join(outdir, "README.txt"))
    return 0


# ---- reading a stick the way U-Boot 2.5.3.0-T48S does ----
# From the disassembly: usb_detect (0x03B2329C) asks for partition 1, 2, 3, 4 of the
# first storage device and takes the first that the DOS partition code (0x03B27490)
# accepts; it never tries the whole device, so a stick with no partition table does
# not work. The FAT code then wants the "FAT" or "FAT32" label in the boot sector.

def _le16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _le32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def check_stick(path, model="T48S", compare_dir=None):
    """(ok, [messages]) for a stick or an image of one."""
    msg = []
    with open(path, "rb") as f:
        def sector(n, count=1):
            f.seek(n * 512)
            return f.read(512 * count)

        s0 = sector(0)
        if len(s0) < 512 or s0[510:512] != b"\x55\xaa":
            return False, ["sector 0 has no 55 AA signature: no partition table (U-Boot: \"Usb failed(-4)\")"]
        looks_fat = s0[54:57] == b"FAT" or s0[82:87] == b"FAT32"
        part = None
        count = 0
        for n in range(1, 5):
            e = s0[446 + 16 * (n - 1):446 + 16 * n]
            if e[4]:
                count += 1
            # 0x03B2752C: boot flag 00 or 80, a type, not an extended partition
            if part is None and not (e[0] & 0x7F) and e[4] and e[4] not in (0x05, 0x0F, 0x85):
                part = (n, e[4], _le32(e, 8), _le32(e, 12))
        if part is None:
            if looks_fat:
                return False, ["the stick is formatted with no partition table (FAT straight on the device). "
                               "U-Boot only looks inside partitions 1 to 4: \"Usb failed(-4)\". "
                               "Make an MBR partition table with one FAT32 partition"]
            return False, ["no usable partition in the MBR (U-Boot: \"Usb failed(-4)\")"]
        n, ptype, start, size = part
        if ptype == 0xEE:
            return False, ["GPT partition table (the MBR only holds its protective entry). U-Boot would take "
                           "the whole stick as partition 1 and find no FAT there. Use an MBR partition table"]
        msg.append("MBR ok: partition %d, type 0x%02x, starts at sector %d, %d MB"
                   % (n, ptype, start, size // 2048))
        if n != 1 or count != 1:
            msg.append("note: %d partitions; U-Boot uses number %d and nothing else. One partition is safest"
                       % (count, n))
        bs = sector(start)
        if bs[3:8] == b"EXFAT":
            return False, msg + ["the partition is exFAT. U-Boot reads FAT32 and FAT16 only"]
        if bs[3:7] == b"NTFS":
            return False, msg + ["the partition is NTFS. U-Boot reads FAT32 and FAT16 only"]
        if len(bs) < 512 or bs[510:512] != b"\x55\xaa" or not (bs[54:57] == b"FAT" or bs[82:87] == b"FAT32"):
            return False, msg + ["no FAT filesystem in the partition (U-Boot: \"No valid FAT fs found\")"]
        bps, spc, rsvd, nfats, rootent = _le16(bs, 11), bs[13], _le16(bs, 14), bs[16], _le16(bs, 17)
        total = _le16(bs, 19) or _le32(bs, 32)
        fat32 = bs[82:87] == b"FAT32"
        fatsz = _le32(bs, 36) if fat32 else _le16(bs, 22)
        if bps != 512:
            return False, msg + ["FAT sector size %d: U-Boot wants it equal to the stick's, 512" % bps]
        if spc * 512 > 65536:
            return False, msg + ["cluster size %d: over U-Boot's 64 KB limit" % (spc * 512)]
        rootsecs = (rootent * 32 + 511) // 512
        data0 = rsvd + nfats * fatsz + rootsecs
        clusters = (total - data0) // spc
        kind = "FAT32" if fat32 else ("FAT16" if clusters >= 4085 else "FAT12")
        msg.append("%s ok: %d KB clusters, %d clusters" % (kind, spc * 512 // 1024, clusters))
        if kind == "FAT12":
            msg.append("note: FAT12 (a very small partition); FAT32 or FAT16 is the tested choice")
        fat = sector(start + rsvd, fatsz)

        def nxt(c):
            if fat32:
                return _le32(fat, c * 4) & 0x0FFFFFFF
            if kind == "FAT16":
                return _le16(fat, c * 2)
            v = _le16(fat, c * 3 // 2)
            return (v >> 4) if c & 1 else (v & 0xFFF)

        eoc = 0x0FFFFFF8 if fat32 else (0xFFF8 if kind == "FAT16" else 0xFF8)

        def chain(c):
            out = []
            while 2 <= c < eoc and len(out) <= clusters:
                out.append(c)
                c = nxt(c)
            return out

        def read_clusters(cl):
            return b"".join(sector(start + data0 + (c - 2) * spc, spc) for c in cl)

        root = read_clusters(chain(_le32(bs, 44))) if fat32 else sector(start + rsvd + nfats * fatsz, rootsecs)
        found = {}
        for i in range(0, len(root), 32):
            e = root[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] & 0x08:      # deleted, volume label or long-name piece
                continue
            # U-Boot compares the lower-cased 8.3 name with the lower-cased name it wants
            short = e[0:8].decode("latin1").rstrip() + ("." + e[8:11].decode("latin1").rstrip()
                                                        if e[8:11].strip() else "")
            found[short.lower()] = ((_le16(e, 20) << 16 | _le16(e, 26)) if fat32 else _le16(e, 26),
                                    _le32(e, 28), bool(e[11] & 0x10))
        ok = True
        contents = {}
        for ext in ("rom", "bin", "rfs"):
            name = "%s.%s" % (model, ext)
            if name.lower() not in found or found[name.lower()][2]:
                msg.append("MISSING: %s is not in the top folder of the stick" % name)
                ok = False
                continue
            first, length, _ = found[name.lower()]
            cl = chain(first)
            data = read_clusters(cl)[:length]
            if len(data) != length:
                msg.append("DAMAGED: %s is %d bytes in the folder but its chain holds %d" % (name, length, len(data)))
                ok = False
                continue
            contents[ext] = data
            pieces = 1 + sum(1 for a, b in zip(cl, cl[1:]) if b != a + 1)
            msg.append("%s: %d bytes, sha256 %s%s" % (name, length, hashlib.sha256(data).hexdigest()[:16],
                                                    "" if pieces == 1 else ", in %d pieces" % pieces))
            if compare_dir:
                want = open(os.path.join(compare_dir, name), "rb").read()
                if want != data:
                    msg.append("DIFFERENT: %s on the stick is not the file in %s" % (name, compare_dir))
                    ok = False
    if "rom" in contents:
        rok, why = check_rom(contents["rom"], board_hwid=MODEL_HWID.get(model, 65))
        msg.append("%s.rom: %s" % (model, why[-1]))
        ok = ok and rok
    if "bin" in contents:
        kok, why, _ = check_bin(contents["bin"])      # either kernel
        msg.append("%s.bin: %s" % (model, why[-1] if not kok else "; ".join(why[:2])))
        ok = ok and kok
    if "rfs" in contents and len(contents["rfs"]) > RFS_SIZE:
        msg.append("%s.rfs is over 8 MB" % model)
        ok = False
    return ok, msg

USAGE = """usage:
  mkliveimage.py usb DIR bin=T48S.bin system=system.img|rfs=T48S.rfs [model=T48S] [kernel=3.6|6.18]
  mkliveimage.py checkstick /dev/sdX|IMAGE [DIR] [model=T48S]
  mkliveimage.py build OUTDIR bin=T48S.bin system=system.img|rfs=T48S.rfs [model=T48S] [kernel=3.6|6.18]
  mkliveimage.py checkbin T48S.bin [kernel=3.6|6.18]
  mkliveimage.py rfs SYSTEM.IMG T48S.rfs
  mkliveimage.py check ROM [board_hwid]"""


def main(argv):
    opts = dict(a.split("=", 1) for a in argv[2:] if "=" in a)
    args = [a for a in argv[2:] if "=" not in a]
    cmd = argv[1] if len(argv) > 1 else ""
    model = opts.get("model", "T48S")
    kern = opts.get("kernel")
    if kern is not None and kern not in KERNELS:
        print("kernel= takes %s" % " or ".join(KERNELS), file=sys.stderr)
        return 2
    if cmd == "checkbin" and args:
        ok, why, _ = check_bin(open(args[0], "rb").read(), kern)
        for m in why[:-1] if not ok else why:
            print("  " + m)
        if not ok:
            print("FAIL: " + why[-1])
        print("PASS" if ok else "REJECTED")
        return 0 if ok else 1
    if cmd == "check" and args:
        data = open(args[0], "rb").read()
        hwid = int(args[1]) if len(args) > 1 else 65
        ok, why = check_rom(data, board_hwid=hwid)
        for m in why:
            print(("  " if ok or m is not why[-1] else "FAIL: ") + m)
        print("PASS" if ok else "REJECTED")
        return 0 if ok else 1
    if cmd in ("build", "usb") and args:
        r = write_set(args[0], opts.get("bin"), opts.get("rfs"), opts.get("system") or opts.get("cpio"), model, readme=cmd == "usb", kern=kern or DEFAULT_KERNEL)
        if r:
            return r
        if cmd == "usb":
            print("\nPut these on a stick:  sudo tools/mkusbstick.sh /dev/sdX %s" % args[0])
            print("(or copy the four files to the top folder of a stick formatted as docs/live-image.md says)")
        else:
            print("\nServe with:  sudo tools/tftpd.py %s 69 --rom-once" % args[0])
        return 0
    if cmd == "rfs" and len(args) == 2:
        try:
            data, what = make_rfs(open(args[0], "rb").read())
        except ValueError as e:
            print("%s: %s" % (args[0], e), file=sys.stderr)
            return 2
        open(args[1], "wb").write(data)
        print("wrote %s (%d bytes): %s" % (args[1], len(data), what))
        return 0
    if cmd == "checkstick" and args:
        ok, why = check_stick(args[0], model, args[1] if len(args) > 1 else None)
        for m in why[:-1] if not ok else why:
            print("  " + m)
        if not ok and why:
            print(("FAIL: " if not why[-1].isupper() else "  ") + why[-1])
        print("PASS: U-Boot should find and start these files" if ok else "REJECTED")
        return 0 if ok else 1
    print(__doc__)
    print(USAGE)
    return 1

if __name__ == "__main__":
    sys.exit(main(sys.argv))
