#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""DTMF digits heard in a recording, for the in-band test of sip/test/phone-test.sh
(issue #77, shell/tests/phone/23-dtmf-modes.script).

  dtmf.py WAV DIGITS    exit 0 if the digits found in WAV (16-bit mono) are DIGITS, in order

Each 25 ms block is measured at the eight DTMF frequencies (Goertzel). A digit
is a block whose strongest low and strongest high tone are both well over the
others and the block's level; a run of such blocks counts once. The header's
sizes are not trusted: a recording still being written has none yet.
"""
import math, struct, sys

LOW = (697, 770, 852, 941)
HIGH = (1209, 1336, 1477, 1633)
KEYS = ("123A", "456B", "789C", "*0#D")


def read(path):
    b = open(path, "rb").read()
    rate, at = 16000, 12
    while at + 8 <= len(b):
        cid, size = b[at:at + 4], struct.unpack("<I", b[at + 4:at + 8])[0]
        if cid == b"fmt ":
            rate = struct.unpack("<I", b[at + 12:at + 16])[0]
        if cid == b"data":
            at += 8
            break
        at += 8 + size
    n = (len(b) - at) // 2
    return rate, struct.unpack("<%dh" % n, b[at:at + 2 * n])


def power(x, f, rate):
    c = 2 * math.cos(2 * math.pi * f / rate)
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    return s1 * s1 + s2 * s2 - c * s1 * s2


def digits(rate, x):
    n = rate // 40
    out, last = "", None
    for i in range(0, len(x) - n, n):
        blk = x[i:i + n]
        energy = sum(v * v for v in blk) * n / 2 or 1
        lo = [power(blk, f, rate) for f in LOW]
        hi = [power(blk, f, rate) for f in HIGH]
        a, b = max(range(4), key=lambda k: lo[k]), max(range(4), key=lambda k: hi[k])
        ok = (lo[a] > 4 * sorted(lo)[-2] and hi[b] > 4 * sorted(hi)[-2] and
              (lo[a] + hi[b]) > 0.5 * energy and max(abs(v) for v in blk) > 1000)
        key = KEYS[a][b] if ok else None
        if key and key != last:
            out += key
        last = key
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rate, x = read(sys.argv[1])
    got = digits(rate, x)
    print(f"dtmf.py: heard '{got}', want '{sys.argv[2]}'")
    sys.exit(0 if got == sys.argv[2] else 1)


if __name__ == "__main__":
    main()
