#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-layout.sh: run the flash driver's layout check and its kernel-model rule
# (kernel6/drivers/t48s_nand_layout.h) on this PC. The check decides whether the
# driver writes anything at all on a phone (docs/models.md).
#
#   test-layout.sh [ENV.dataoob]
#
# Environments are made here from tools/t48flash.py's partition table, then
# broken one way at a time. With a dump of a phone's env partition (nandread -o,
# as in a backup), that phone's own environment is tried too.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/t.c" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include "t48s_nand_layout.h"
int main(int argc, char **argv)
{
	unsigned char env[T48S_ENV_LEN + 1], page[64];
	FILE *f;
	size_t n;
	const char *why;

	if (argc == 3 && !strcmp(argv[1], "kernel")) {
		/* argv[2]: "NAME MODEL", an image name and this phone's model (- for none) */
		char name[40], model[8];
		memset(page, 0, sizeof page);
		page[0] = 0x27; page[1] = 0x05; page[2] = 0x19; page[3] = 0x56;
		if (sscanf(argv[2], "%39s %7s", name, model) != 2) return 2;
		if (!strcmp(name, "-yealink-plain")) memcpy(page + 32, "Linux-2.6.27.47", 15);
		else if (!strcmp(name, "-not-an-image")) { memset(page, 0, 4); memcpy(page + 32, "Linux-6.18.65-t48s", 18); }
		else memcpy(page + 32, name, strlen(name) < 32 ? strlen(name) : 32);
		printf("%d\n", t48s_layout_kernel_ok(page, strcmp(model, "-") ? model : NULL));
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "mtdparts")) { printf("%s\n", t48s_layout_mtdparts); return 0; }
	if (argc == 4 && !strcmp(argv[1], "var") && (f = fopen(argv[2], "rb"))) {
		/* what the driver logs when the layout is not known */
		unsigned long vl;
		const char *v;
		n = fread(env, 1, T48S_ENV_LEN, f);
		fclose(f);
		v = t48s_layout_var(env, n, argv[3], &vl);
		if (v) printf("%.*s\n", (int)vl, v); else printf("(none)\n");
		return 0;
	}
	if (argc != 2 || !(f = fopen(argv[1], "rb"))) return 2;
	n = fread(env, 1, sizeof env, f);
	fclose(f);
	why = t48s_layout_check(env, n);
	printf("%s\n", why ? why : "matches");
	return 0;
}
EOC
cc -Wall -Wextra -Werror -O2 -I"$HERE/kernel6/drivers" -o "$T/t" "$T/t.c"
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }
N=0
want() { N=$((N + 1)); got=$("$T/t" "$1"); [ "$got" = "$2" ] && pass "$3" || fail "$3: got \"$got\""; }

# the table the driver expects is the table the PC tools use
python3 -I - "$HERE/tools" > "$T/want-mtdparts" <<'EOP'
import sys
sys.path.insert(0, sys.argv[1])
import t48flash
p = ["0x%08X@0x%08X(%s)rw" % (s, o, n) for n, o, s in t48flash.PARTS]
p.insert(9, "0x00000000@0x02720000(boot)rw")
print("mtdparts=bcm_umi_nand.0:" + ",".join(p) + ";")
EOP
[ "$("$T/t" mtdparts)" = "$(cat "$T/want-mtdparts")" ] && pass "the expected mtdparts is tools/t48flash.py's table" || fail "mtdparts constant"

# environments: one good one, then each thing that must refuse
python3 -I - "$T" <<'EOP'
import struct, sys, zlib
out = sys.argv[1]
mtdparts = open(out + "/want-mtdparts").read().strip()
good = [("mtdparts", mtdparts), ("bootcmd", "run bootnand"), ("bootdelay", "1"),
        ("bootnand", "setenv bootargs eth=$(ethaddr) noinitrd root=/dev/mtdblock8 ro ${mtdparts}; setenv autostart yes; nboot 0x02680000 0 0x015A0000"),
        ("bootram", "setenv autostart yes; nboot 0x02680000 0 0x00420000"), ("baudrate", "115200")]
def block(vs, crc=True, pad=b"\0", raw=None):
    data = raw if raw is not None else b"".join(("%s=%s" % kv).encode() + b"\0" for kv in vs) + b"\0"
    data = data + pad * (4092 - len(data))
    c = zlib.crc32(data) if crc else zlib.crc32(data) ^ 1
    return struct.pack("<I", c) + data
def put(name, b):
    open("%s/%s" % (out, name), "wb").write(b)
def edit(key, val):
    return [(k, val if k == key else v) for k, v in good]
put("good", block(good))
put("good-ff", block(good, pad=b"\xff"))
put("crc", block(good, crc=False))
put("short", block(good)[:4095])
put("erased", b"\xff" * 4096)
put("offset", block(edit("mtdparts", mtdparts.replace("0x04AE0000@0x02720000(app)", "0x04AE0000@0x02740000(app)"))))
put("size", block(edit("mtdparts", mtdparts.replace("0x00280000@0x015A0000(kernel)", "0x00300000@0x015A0000(kernel)"))))
put("lower", block(edit("mtdparts", mtdparts.replace("0x04AE0000", "0x04ae0000"))))
put("prefix", block(edit("mtdparts", mtdparts.replace("mtdparts=", "", 1))))
put("tail", block(edit("mtdparts", mtdparts + "x")))
put("nboot", block(edit("bootnand", good[3][1].replace("0x015A0000", "0x00420000"))))
put("nbootaddr", block(edit("bootnand", good[3][1].replace("0x02680000 0", "0x02a80000 0"))))
put("nbootafter", block(edit("bootnand", good[3][1] + "; reset")))
put("noauto", block(edit("bootnand", good[3][1].replace("setenv autostart yes; ", ""))))
put("bootcmd", block(edit("bootcmd", "run bootram")))
put("dup", block(good + [("mtdparts", mtdparts)]))
put("nomtd", block([kv for kv in good if kv[0] != "mtdparts"]))
put("nocmd", block([kv for kv in good if kv[0] != "bootcmd"]))
put("nonand", block([kv for kv in good if kv[0] != "bootnand"]))
raw = b"".join(("%s=%s" % kv).encode() + b"\0" for kv in good)
raw = raw + b"x" * (4092 - len(raw))
put("unterminated", struct.pack("<I", zlib.crc32(raw)) + raw)
EOP
want "$T/good" matches "a T48S environment matches"
want "$T/good-ff" matches "the same, the rest of the block erased"
want "$T/crc" "no environment (CRC wrong)" "a wrong CRC refuses"
want "$T/short" "not one environment block" "a short read refuses"
want "$T/erased" "no environment (CRC wrong)" "an erased block refuses"
want "$T/offset" "mtdparts is not this layout" "app 128 KB further on refuses"
want "$T/size" "mtdparts is not this layout" "a larger kernel partition refuses"
want "$T/lower" "mtdparts is not this layout" "the same numbers written otherwise refuse"
want "$T/prefix" "mtdparts is not this layout" "mtdparts without its own name refuses"
want "$T/tail" "mtdparts is not this layout" "anything after the table refuses"
want "$T/nboot" "bootnand does not start the kernel partition" "a boot from smkern refuses"
want "$T/nbootaddr" "bootnand does not start the kernel partition" "a read to another address refuses"
want "$T/nbootafter" "bootnand does not start the kernel partition" "a command after nboot refuses"
want "$T/noauto" "bootnand does not start the kernel partition" "nboot without autostart refuses"
want "$T/bootcmd" "bootcmd is not run bootnand" "another boot command refuses"
want "$T/dup" "mtdparts is not this layout" "mtdparts twice refuses"
want "$T/nomtd" "no mtdparts" "no mtdparts refuses"
want "$T/nocmd" "no bootcmd" "no bootcmd refuses"
want "$T/nonand" "no bootnand" "no bootnand refuses"
want "$T/unterminated" "environment not terminated" "an environment that runs off the block refuses"
[ "$("$T/t" var "$T/nboot" bootcmd)" = "run bootnand" ] && [ "$("$T/t" var "$T/offset" mtdparts)" = "$(sed 's/0x02720000(app)/0x02740000(app)/' "$T/want-mtdparts")" ] &&
    [ "$("$T/t" var "$T/nomtd" mtdparts)" = "(none)" ] && pass "what the log shows of an environment that does not match" || fail "variables for the log"

# the kernel-model rule
k() { N=$((N + 1)); got=$("$T/t" kernel "$1"); [ "$got" = "$2" ] && pass "$3" || fail "$3: got $got"; }
k "Linux-6.18.65-t46s t46s" 1 "a T46S kernel on a T46S"
k "Linux-6.18.65-t48s t48s" 1 "a T48S kernel on a T48S"
k "Linux-6.18.65-t48s t46s" 0 "a T48S kernel on a T46S is refused"
k "Linux-6.18.65-t46s t48s" 0 "a T46S kernel on a T48S is refused"
k "Linux-6.18.65-t42s t46s" 0 "a kernel for any other model is refused"
k "Linux-6.18.65-t46s -" 0 "a model kernel where the model is not known is refused"
k "-yealink-plain t46s" 1 "Yealink's own kernel (a restore) is not this rule's business"
k "-not-an-image t46s" 1 "something that is not an image is not this rule's business"

if [ -n "$1" ]; then
    python3 -I -c "
import sys
d = open(sys.argv[1], 'rb').read(2 * 2112)
open(sys.argv[2], 'wb').write(d[:2048] + d[2112:2112 + 2048])" "$1" "$T/dump"
    want "$T/dump" matches "this phone's own environment ($1) matches"
fi
echo "layout check: all $N cases pass"
