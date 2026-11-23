#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mkinitramfs.py OUT BASE.cpio[.gz] [--overlay DIR] [--file NAME=PATH[:MODE]] ...

Make the kernel's built-in root filesystem: BASE (a newc cpio, gzipped or not,
from Buildroot) with more files added on the end as further cpio archives. The
kernel unpacks them in turn, so a later file replaces an earlier one of the
same name. Everything added is owned by root.
"""
import gzip, os, sys

def entry(ino, name, mode, nlink, body=b""):
    name = name.encode() + b"\0"
    hdr = "070701" + "".join("%08X" % v for v in
        (ino, mode, 0, 0, nlink, 0, len(body), 0, 0, 0, 0, len(name), 0))
    rec = hdr.encode() + name
    rec += b"\0" * (-len(rec) % 4)
    return rec + body + b"\0" * (-len(body) % 4)

def main():
    out_path, base = sys.argv[1:3]
    args = sys.argv[3:]
    data = open(base, "rb").read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    out = bytearray(data)
    files = []          # (name, mode, body or None for a directory)
    seen_dirs = set()
    def add_dirs(name):
        parts = name.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            d = "/".join(parts[:i])
            if d not in seen_dirs:
                seen_dirs.add(d)
                files.append((d, 0o40755, None))
    while args:
        a = args.pop(0)
        if a == "--overlay":
            root = args.pop(0)
            for dp, dns, fns in sorted(os.walk(root)):
                for fn in sorted(fns):
                    p = os.path.join(dp, fn)
                    name = os.path.relpath(p, root)
                    add_dirs(name)
                    mode = 0o100755 if os.access(p, os.X_OK) else 0o100644
                    files.append((name, mode, open(p, "rb").read()))
        elif a == "--file":
            name, path = args.pop(0).split("=", 1)
            mode = 0o100644
            if ":" in path:
                path, m = path.rsplit(":", 1)
                mode = 0o100000 | int(m, 8)
            add_dirs(name)
            files.append((name, mode, open(path, "rb").read()))
        else:
            sys.exit("unknown argument " + a)
    if files:
        out += b"\0" * (-len(out) % 4)
        for ino, (name, mode, body) in enumerate(files, 1):
            out += entry(ino, name, mode, 2 if body is None else 1, body or b"")
        out += entry(0, "TRAILER!!!", 0, 1)
    open(out_path, "wb").write(out)
    print("initramfs: %s, %d bytes, %d files added" % (out_path, len(out), len(files)))

main()
