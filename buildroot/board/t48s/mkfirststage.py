#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mkfirststage.py TARGET_DIR OUT.cpio

Make the first stage (issue #17, docs/boot.md): a small cpio archive for the
kernel's built-in initramfs, holding busybox and dropbear from Buildroot's
target folder, the init script and what rescue needs. No root needed: the
archive, its device node included, is written directly.
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
target, out = sys.argv[1:3]
arc, ino = bytearray(), [0]


def entry(name, mode, body=b"", nlink=1, rdev=(0, 0)):
    ino[0] += 1
    n = name.encode() + b"\0"
    hdr = "070701" + "".join("%08X" % v for v in (
        ino[0], mode, 0, 0, nlink, 0, len(body), 0, 0, rdev[0], rdev[1], len(n), 0))
    rec = hdr.encode() + n
    rec += b"\0" * (-len(rec) % 4)
    arc.extend(rec + body + b"\0" * (-len(body) % 4))


def read(*p):
    return open(os.path.join(*p), "rb").read()


for d in ("bin", "dev", "etc", "usr", "usr/bin", "usr/sbin", "root"):
    entry(d, 0o40755, nlink=2)
entry("dev/console", 0o20600, rdev=(5, 1))
entry("init", 0o100755, read(HERE, "firststage", "init"))
entry("etc/udhcpc.script", 0o100755, read(HERE, "firststage", "udhcpc.script"))
entry("bin/busybox", 0o100755, read(target, "bin", "busybox"))
entry("usr/sbin/dropbear", 0o100755, read(target, "usr", "sbin", "dropbear"))
entry("usr/bin/dropbearkey", 0o120777, b"../sbin/dropbear")
entry("etc/passwd", 0o100644, b"root:x:0:0:root:/root:/bin/sh\n")
entry("etc/group", 0o100644, b"root:x:0:\n")
entry("etc/shadow", 0o100600, b"root:*:::::::\n")
entry("etc/shells", 0o100644, b"/bin/sh\n")
ino[0] = -1
entry("TRAILER!!!", 0)
open(out, "wb").write(arc)
print("first stage: %s, %d bytes" % (out, len(arc)))
