#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mkliveimage.py: build (and check) the "try it without flashing" live-image set.

Issue #7. U-Boot's recovery mode (hold Speakerphone at power-on, menu option 1
TFTP) fetches three files named after the board (T48S.bin, T48S.rfs, T48S.rom),
checks only the 128-byte head of the .rom, and then boots the .bin + .rfs. It
writes nothing to the phone's flash except its own environment (see below).

So a shareable live image is: our kernel as T48S.bin, our root filesystem as
T48S.rfs, and a SMALL CRAFTED T48S.rom whose header passes U-Boot's check
WITHOUT being a copy of Yealink's firmware (which we may not redistribute).

This tool does two jobs:

  mkliveimage.py check  ROM            emulate U-Boot's romcheck on a .rom file
  mkliveimage.py build  OUTDIR  [opts] craft T48S.rom and assemble the set

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


CONF_TEMPLATE = """\
# T48S.conf: settings for the live image (issue #7). Plain text, key = value.
# Read once at boot and held in RAM only; nothing is written to the phone's
# flash. Lost at power off. Lines starting with # are ignored.
#
# The live image looks for this file by TFTP from the server that answered DHCP
# (option 66 / the boot server), under the same name as the other files, after
# it has an address. If it is not found the phone still boots; it just has no
# SIP account and SSH stays off.

# --- SIP account (optional) ---
# sip.user     = 1001
# sip.domain   = pbx.example.net
# sip.password = changeme
# sip.name     = Live T48S

# --- SSH (optional) ---
# One authorized key per line turns SSH on at boot (docs/access.md). Without any
# key, SSH stays off and you turn it on at the phone's screen.
# ssh.authorized_key = ssh-ed25519 AAAA... you@host

# --- apps to fetch on boot (optional) ---
# app.feed = https://m0lte.github.io/yealink-t48s-apps/index.json
# app.install = waterfall links
"""


def build(outdir, bin_path, rfs_path, model="T48S"):
    os.makedirs(outdir, exist_ok=True)
    rom = craft_rom(model)
    ok, why = check_rom(rom, board_hwid=MODEL_HWID.get(model, 65))
    if not ok:
        print("refusing to write a ROM our own check rejects:", why[-1], file=sys.stderr)
        return 2
    rom_path = os.path.join(outdir, model + ".rom")
    with open(rom_path, "wb") as f:
        f.write(rom)
    print("wrote %s (%d bytes): %s" % (rom_path, len(rom), " / ".join(why)))
    for src, name in ((bin_path, model + ".bin"), (rfs_path, model + ".rfs")):
        dst = os.path.join(outdir, name)
        if src:
            with open(src, "rb") as a, open(dst, "wb") as b:
                b.write(a.read())
            print("copied %s -> %s" % (src, dst))
        elif not os.path.exists(dst):
            print("note: %s not supplied and not present in %s "
                  "(build the kernel/rootfs and drop it in)" % (name, outdir))
    conf = os.path.join(outdir, model + ".conf.example")
    with open(conf, "w") as f:
        f.write(CONF_TEMPLATE)
    print("wrote %s" % conf)
    print("\nServe with:  sudo tools/tftpd.py %s 69 --rom-once" % outdir)
    print("Boot it: hold Speakerphone, reconnect power, choose 1 (TFTP), enter the")
    print("server IP. See docs/live-image.md for the full procedure and the recovery steps.")
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "check":
        data = open(argv[2], "rb").read()
        hwid = int(argv[3]) if len(argv) > 3 else 65
        ok, why = check_rom(data, board_hwid=hwid)
        for m in why:
            print(("  " if ok or m is not why[-1] else "FAIL: ") + m)
        print("PASS" if ok else "REJECTED")
        return 0 if ok else 1
    if len(argv) >= 3 and argv[1] == "build":
        outdir = argv[2]
        opts = dict(a.split("=", 1) for a in argv[3:] if "=" in a)
        return build(outdir, opts.get("bin"), opts.get("rfs"), opts.get("model", "T48S"))
    print(__doc__)
    print("usage:\n  mkliveimage.py check ROM [board_hwid]\n"
          "  mkliveimage.py build OUTDIR [bin=T48S.bin] [rfs=T48S.rfs] [model=T48S]")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
