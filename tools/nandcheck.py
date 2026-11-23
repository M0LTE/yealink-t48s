#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check and compare NAND dumps made with `nandread -o` (issue #5).

  nandcheck.py ecc FILE...   recompute every ECC byte in software and compare
  nandcheck.py cmp A B       compare two dumps of the same partition page by page

A dump is a run of pages, each 2048 data bytes followed by its 64 spare bytes.

The phone's flash controller protects each 512 data bytes with 7 ECC bytes at
spare bytes 9-15 of that step's 16-byte group. The code is a plain binary BCH code:
GF(2^13) with primitive polynomial 0x201b, 4 correctable bits, 52 parity bits. The
data goes in most significant bit first, followed by four zero bits; the parity is
stored big-endian in the low 52 bits of the 7 bytes. A step that was never written
reads as all 0xff, ECC included.
"""
import sys

PAGE, SPARE, STEP, GROUP, ECC_AT, ECC_LEN = 2048, 64, 512, 16, 9, 7
M, PRIM, T = 13, 0x201B, 4


def generator():
    """Generator polynomial of the BCH code, as a bit mask."""
    n = (1 << M) - 1
    exp, log, x = [0] * (2 * n), [0] * (n + 1), 1
    for i in range(n):
        exp[i], log[x] = x, i
        x <<= 1
        if x >> M:
            x ^= PRIM
    exp[n:] = exp[:n]
    g, seen = 1, set()
    for i in range(1, 2 * T + 1):
        cls, c = [], i
        while c not in cls:
            cls.append(c)
            c = c * 2 % n
        if min(cls) in seen:
            continue
        seen.add(min(cls))
        poly = [1]                      # product of (x - a^j) over the class
        for j in cls:
            new = [0] * (len(poly) + 1)
            for d, co in enumerate(poly):
                new[d + 1] ^= co
                if co:
                    new[d] ^= exp[log[co] + j]
            poly = new
        mp = sum(co << d for d, co in enumerate(poly))
        g = _mul(g, mp)
    return g


def _mul(a, b):
    r, s = 0, 0
    while a:
        if a & 1:
            r ^= b << s
        a >>= 1
        s += 1
    return r


G = generator()
BITS = G.bit_length() - 1               # 52
MASK = (1 << BITS) - 1
LOW = G & MASK
TABLE = []
for _b in range(256):
    _r = _b << (BITS - 8)
    for _ in range(8):
        _r = ((_r << 1) ^ LOW) & MASK if _r >> (BITS - 1) & 1 else (_r << 1) & MASK
    TABLE.append(_r)


def ecc(data):
    """The 7 ECC bytes the controller stores for 512 data bytes."""
    r = 0
    for byte in data:
        r = ((r << 8) & MASK) ^ TABLE[(r >> (BITS - 8)) ^ byte]
    for _ in range(4):                  # the four zero bits after the data
        r = ((r << 1) ^ LOW) & MASK if r >> (BITS - 1) & 1 else (r << 1) & MASK
    return r.to_bytes(ECC_LEN, "big")


def pages(path):
    d = open(path, "rb").read()
    if len(d) % (PAGE + SPARE):
        sys.exit("%s: not a whole number of 2112-byte pages" % path)
    for i in range(0, len(d), PAGE + SPARE):
        yield d[i:i + PAGE], d[i + PAGE:i + PAGE + SPARE]


def cmd_ecc(files):
    worst = 0
    for f in files:
        ok = bad = erased = 0
        for n, (data, spare) in enumerate(pages(f)):
            for s in range(PAGE // STEP):
                e = spare[s * GROUP + ECC_AT:(s + 1) * GROUP]
                d = data[s * STEP:(s + 1) * STEP]
                if e == b"\xff" * ECC_LEN and d == b"\xff" * STEP:
                    erased += 1
                elif ecc(d) == e:
                    ok += 1
                else:
                    bad += 1
                    if bad <= 5:
                        print("  page %d step %d: stored %s, computed %s" % (n, s, e.hex(), ecc(d).hex()))
        print("%s: %d steps match, %d differ, %d never written" % (f, ok, bad, erased))
        worst |= bad
    return 1 if worst else 0


def cmd_cmp(a, b):
    pa, pb = list(pages(a)), list(pages(b))
    if len(pa) != len(pb):
        print("different sizes: %d and %d pages" % (len(pa), len(pb)))
    dd = od = 0
    for n, ((da, oa), (db, ob)) in enumerate(zip(pa, pb)):
        if da != db:
            dd += 1
            if dd <= 5:
                bits = sum(bin(x ^ y).count("1") for x, y in zip(da, db))
                print("  page %d: data differs (%d bits)" % (n, bits))
        if oa != ob:
            od += 1
            if od <= 5:
                print("  page %d: spare differs\n    %s\n    %s" % (n, oa.hex(), ob.hex()))
    print("%d pages: %d with different data, %d with different spare bytes" % (min(len(pa), len(pb)), dd, od))
    return 1 if dd or od or len(pa) != len(pb) else 0


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "ecc":
        sys.exit(cmd_ecc(sys.argv[2:]))
    if len(sys.argv) == 4 and sys.argv[1] == "cmp":
        sys.exit(cmd_cmp(sys.argv[2], sys.argv[3]))
    sys.exit(__doc__)
