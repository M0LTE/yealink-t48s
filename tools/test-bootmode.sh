#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-bootmode.sh [USERENV-DUMP...]: run the kernel's userenv code
# (kernel/drivers/t48s_bootmode_env.h) on this PC. With no arguments it tests
# made-up blocks; give it dumps of the userenv partition (dumps/mtd3-userenv.bin)
# to check those too. Compares every result with an independent Python version.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/t.c" <<'EOC'
#include <stdio.h>
#include <string.h>
static unsigned int t48s_env_crc(const unsigned char *p, int n)
{
	unsigned int c = ~0U;
	int k;
	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = c >> 1 ^ (c & 1 ? 0xedb88320 : 0);
	}
	return ~c;
}
#include "t48s_bootmode_env.h"
int main(int argc, char **argv)
{
	unsigned char in[T48S_ENV_SIZE], out[T48S_ENV_SIZE];
	char val[32];
	FILE *f = argc == 3 ? fopen(argv[1], "rb") : 0;
	if (!f || fread(in, 1, sizeof in, f) != sizeof in) return 2;
	fclose(f);
	if (!t48s_env_valid(in)) { puts("invalid"); return 0; }
	printf("mode=%s\n", t48s_env_get(in, "BOOTMODE", val, sizeof val) ? val : "none");
	printf("removed=%d\n", t48s_env_remove(in, out, "BOOTMODE"));
	printf("outvalid=%d\n", t48s_env_valid(out));
	f = fopen(argv[2], "wb"); fwrite(out, 1, sizeof out, f); fclose(f);
	return 0;
}
EOC
cc -Wall -Wextra -Werror -O2 -I"$HERE/kernel/drivers" -o "$T/t" "$T/t.c"
python3 - "$T" "$@" <<'EOP'
import os, subprocess, struct, sys, zlib
T, dumps = sys.argv[1], sys.argv[2:]
def block(vars, junk=b""):
    body = b"".join(v + b"\0" for v in vars) + b"\0" + junk
    body = body.ljust(4092, b"\0")
    return struct.pack("<I", zlib.crc32(body)) + body
def parse(b):
    if zlib.crc32(b[4:]) != struct.unpack("<I", b[:4])[0]: return None
    end = b.find(b"\0\0", 4) if b[4] else 3
    if end < 0: return None
    return [v for v in b[4:end].split(b"\0") if v]
base = [b"hostname=T48S", b"phoneoem=NULL", b"phonesid=0", b"vercur=82"]
cases = {
    "none": block(base),
    "usb-last": block(base + [b"BOOTMODE=usbmode"]),
    "resume-first": block([b"BOOTMODE=resumemode"] + base),
    "middle-with-junk": block(base[:2] + [b"BOOTMODE=safemode"] + base[2:], b"DE=resumemode\0old"),
    "lookalikes": block([b"XBOOTMODE=1", b"BOOTMODEX=2", b"BOOTMODE=usbmode", b"A=BOOTMODE=3"]),
    "only": block([b"BOOTMODE=usbmode"]),
    "empty": block([]),
    "bad-crc": b"\0\0\0\0" + block(base)[4:],
    "no-end": struct.pack("<I", zlib.crc32(b"a" * 4092)) + b"a" * 4092,
    "erased": b"\xff" * 4096,
}
for d in dumps:
    cases[d[-22:]] = open(d, "rb").read()[:4096]
bad = 0
for name, b in cases.items():
    open(T + "/in", "wb").write(b)
    got = subprocess.run([T + "/t", T + "/in", T + "/out"], capture_output=True, text=True).stdout.split()
    want_vars = parse(b)
    if want_vars is None:
        ok = got == ["invalid"]
        what = "invalid, as it should be"
    else:
        mode = [v[9:].decode() for v in want_vars if v.startswith(b"BOOTMODE=")]
        kept = [v for v in want_vars if not v.startswith(b"BOOTMODE=")]
        out = open(T + "/out", "rb").read()
        ok = (got == ["mode=" + (mode[0] if mode else "none"), "removed=%d" % len(mode), "outvalid=1"]
              and out == block(kept))
        what = "mode %s, %d variables kept" % (mode[0] if mode else "none", len(kept))
    print("%-4s %-22s %s" % ("ok" if ok else "FAIL", name, what if ok else got))
    bad += not ok
sys.exit(1 if bad else 0)
EOP
echo "userenv code: all cases pass"
