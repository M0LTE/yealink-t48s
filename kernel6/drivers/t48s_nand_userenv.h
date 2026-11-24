/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Yealink T48S flash driver: the one write outside the kernel and app
 * partitions (issue #7, docs/live-image.md). Plain functions with no kernel
 * dependencies, so that tools/test-userenv.sh can run them on a PC.
 *
 * U-Boot's recovery mode leaves BOOTMODE set in its "userenv" partition, and
 * Yealink's firmware resets its own settings when it next starts and finds it.
 * The live image takes the flag out again. For that, and nothing else,
 * t48s_nand_userenv_rewrite() may erase ONE block of userenv and program its
 * first two pages with a new 4 KB environment block that the kernel built
 * itself (kernel/drivers/t48s_bootmode.c). No byte of it comes from userspace.
 *
 * It is not a fourth range on the allow-list. It is a window that is closed
 * except inside that one function:
 *
 *   closed    nothing in userenv can be erased or written (every start, and
 *             always outside the function)
 *   erase     exactly one erase, of exactly the block named, and no program
 *   program   exactly pages 0 and 1 of that block, and no erase
 *
 * The block must be a whole block inside userenv; anything else cannot be
 * opened at all. The three layers of the driver each ask t48s_userenv_ok() for
 * themselves, as they ask the allow-list. A bad block marker can never be
 * written through the window: those paths ask the allow-list alone.
 */
#ifndef T48S_NAND_USERENV_H
#define T48S_NAND_USERENV_H

#define T48S_UENV_OFS		0x00220000LL	/* userenv on the chip */
#define T48S_UENV_SIZE		0x00100000LL
#define T48S_UENV_BLOCK		0x00020000LL
#define T48S_UENV_PAGE		0x00000800LL
#define T48S_UENV_LEN		0x00001000LL	/* the environment block: two pages */

enum { T48S_UENV_CLOSED, T48S_UENV_ERASE, T48S_UENV_PROGRAM };

struct t48s_uenv_window {
	long long block;	/* chip offset of the one block; -1 when closed */
	int phase;
};

/* may this block be opened at all: a whole block of userenv, nothing else */
static int t48s_userenv_block_ok(long long block)
{
	return block >= T48S_UENV_OFS && block + T48S_UENV_BLOCK <= T48S_UENV_OFS + T48S_UENV_SIZE &&
	       !(block & (T48S_UENV_BLOCK - 1));
}

/* Set the window. Returns 0, or -1 (and closes it) for anything but a block of
 * userenv and one of the two open phases. */
static int t48s_userenv_set(struct t48s_uenv_window *w, long long block, int phase)
{
	w->block = -1;
	w->phase = T48S_UENV_CLOSED;
	if (phase == T48S_UENV_CLOSED)
		return 0;
	if ((phase != T48S_UENV_ERASE && phase != T48S_UENV_PROGRAM) || !t48s_userenv_block_ok(block))
		return -1;
	w->block = block;
	w->phase = phase;
	return 0;
}

/* may [ofs, ofs + len) be erased (erase phase) or programmed (program phase)? */
static int t48s_userenv_ok(const struct t48s_uenv_window *w, long long ofs, long long len)
{
	if (len <= 0 || !t48s_userenv_block_ok(w->block))
		return 0;
	switch (w->phase) {
	case T48S_UENV_ERASE:
		return ofs == w->block && len == T48S_UENV_BLOCK;
	case T48S_UENV_PROGRAM:
		/* whole pages among the first two, never more */
		return ofs >= w->block && !(ofs & (T48S_UENV_PAGE - 1)) &&
		       (len == T48S_UENV_PAGE || len == T48S_UENV_LEN) &&
		       ofs + len <= w->block + T48S_UENV_LEN;
	}
	return 0;
}

#endif
