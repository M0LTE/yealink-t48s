#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-userenv.sh: run the flash driver's userenv window
# (kernel6/drivers/t48s_nand_userenv.h) on this PC, against every block of the
# chip and an independent statement of the rule. The window is the one way the
# driver writes outside the kernel and app partitions (docs/live-image.md).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/t.c" <<'EOC'
#include <stdio.h>
#include "t48s_nand_userenv.h"
#define CHIP 0x08000000LL
#define BLOCK 0x20000LL
#define PAGE 0x800LL
static long n, bad;
static void want(int got, int should, const char *what, long long a, long long b)
{
	n++;
	if (!got != !should) { bad++; if (bad < 20) printf("FAIL %s at 0x%llx len 0x%llx: got %d\n", what, a, b, got); }
}
int main(void)
{
	struct t48s_uenv_window w;
	long long b, o, len, wb;
	int ph;
	static const long long lens[] = { -1, 0, 1, 512, 1024, PAGE, PAGE + 1, 2 * PAGE, 3 * PAGE, 4 * PAGE, BLOCK - PAGE, BLOCK, BLOCK + PAGE, 2 * BLOCK };

	/* 1. only whole blocks of userenv can be opened, and only for erase or program */
	for (b = -BLOCK; b <= CHIP; b += PAGE)
		for (ph = -1; ph <= 4; ph++) {
			int in = b >= 0x220000 && b < 0x320000 && b % BLOCK == 0;
			int rc = t48s_userenv_set(&w, b, ph);
			if (ph == T48S_UENV_CLOSED) { want(rc == 0 && w.block == -1, 1, "close", b, ph); continue; }
			want(rc == 0, in && (ph == 1 || ph == 2), "open", b, ph);
			want(w.block == -1 && w.phase == 0, !(in && (ph == 1 || ph == 2)), "left closed after a refused open", b, ph);
		}
	/* 2. closed: nothing, anywhere */
	t48s_userenv_set(&w, -1, T48S_UENV_CLOSED);
	for (o = 0; o < CHIP; o += PAGE)
		for (len = 0; len < (long long)(sizeof lens / sizeof lens[0]); len++)
			want(t48s_userenv_ok(&w, o, lens[len]), 0, "closed", o, lens[len]);
	/* 3. open at each block of userenv in each phase: every page offset of the chip
	 * (and offsets inside a page near the window), every length */
	for (wb = 0x220000; wb < 0x320000; wb += BLOCK)
		for (ph = 1; ph <= 2; ph++) {
			t48s_userenv_set(&w, wb, ph);
			for (o = 0; o < CHIP; o += (o >= wb - BLOCK && o < wb + 2 * BLOCK) ? 256 : PAGE)
				for (len = 0; len < (long long)(sizeof lens / sizeof lens[0]); len++) {
					long long l = lens[len];
					int should = ph == 1 ? (o == wb && l == BLOCK)
						: ((o == wb && (l == PAGE || l == 2 * PAGE)) || (o == wb + PAGE && l == PAGE));
					want(t48s_userenv_ok(&w, o, l), should, ph == 1 ? "erase phase" : "program phase", o, l);
				}
		}
	/* 4. a window struct that was never set, or holds rubbish, lets nothing through */
	w.block = 0; w.phase = T48S_UENV_ERASE;
	want(t48s_userenv_ok(&w, 0, BLOCK), 0, "rubbish window: the loader", 0, BLOCK);
	w.block = 0x015a0000; w.phase = T48S_UENV_PROGRAM;
	want(t48s_userenv_ok(&w, 0x015a0000, PAGE), 0, "rubbish window: kernel", 0x015a0000, PAGE);
	w.block = 0x220000; w.phase = 9;
	want(t48s_userenv_ok(&w, 0x220000, BLOCK), 0, "rubbish window: phase", 0x220000, BLOCK);
	printf("%ld checks, %ld wrong\n", n, bad);
	return bad != 0;
}
EOC
cc -Wall -Wextra -Werror -O2 -I"$HERE/kernel6/drivers" -o "$T/t" "$T/t.c"
"$T/t"
echo "userenv window: all cases pass"
