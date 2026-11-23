// SPDX-License-Identifier: GPL-2.0
/*
 * Yealink T48S: the NAND flash (kernel6, issue #51; issues #5, #29 for the design).
 *
 * The SoC's UMI block drives a Winbond 128 MB chip (ID ef f1, 2048-byte pages, 64
 * spare bytes, 128 KB blocks) and has a hardware BCH engine, used the way the
 * phone's U-Boot and Yealink's kernel use it:
 *
 *   4 correctable bits per 512 data bytes, 7 ECC bytes each
 *   spare area, four 16-byte groups, one per 512 bytes of data:
 *     byte 0      bad block marker in the first group, free in the others
 *     bytes 1-8   free (Yealink's yaffs2 keeps its tags here), not covered by the ECC
 *     bytes 9-15  ECC
 *
 * This is kernel/drivers/t48s_nand.c (Linux 3.6) ported to the current raw NAND
 * layer, with the same rule and the same checks.
 *
 * READING works everywhere. WRITING AND ERASING work only inside the ranges listed
 * in t48s_nand_ours[] below, which is compiled in and cannot be changed at run
 * time. Everything else, and above all the loader, U-Boot, its two environments,
 * the logo and the safe-mode kernel and root filesystem, cannot be changed by this
 * driver. Three independent layers, each checking the address itself:
 *
 *  1. Every byte for the chip's command, address and data latches passes through
 *     t48s_nand_cmd_ctrl() and t48s_nand_write_buf(). Reads go straight through.
 *     Program (80h) and erase (60h) are held back until all their address bytes
 *     have arrived; they are sent only if that address is in t48s_nand_ours[] and
 *     is the page the write code below announced. The confirm commands (10h, D0h)
 *     and the data bytes are sent only after that. Lock, reset, set-features,
 *     random data input and anything unknown are never sent. Refusals are counted
 *     in /sys/module/t48s_nand/parameters/blocked.
 *  2. The write hooks of the NAND layer (page write, bad block marker) and the
 *     command hook that erases check the page against t48s_nand_ours[] before
 *     sending anything. Raw writes and writes of spare bytes alone (other than a
 *     bad block marker) do not exist.
 *  3. The MTD layer: every partition is registered read-only except those that
 *     are one of the ranges, and the device's write, erase and mark-bad entry
 *     points refuse any address outside them with -EROFS.
 *
 * At start-up, before the flash is registered, t48s_nand_selftest() asks every
 * layer in turn to erase and program places that are not ours and checks that
 * each refuses and that not one byte went to the chip's latches. If any attempt
 * gets through a layer, there is no flash at all.
 *
 * How a page is written:
 *   - the page must read as erased (all ff, spare bytes too) or it is refused;
 *   - the ECC is computed here in software (the code tools/nandcheck.py checks
 *     dumps with, proven against every written page of the stock flash), so what
 *     goes to the chip is fully known before it is sent;
 *   - after programming, the page is read back raw, all 2112 bytes, and compared.
 *     Any difference is an error (-EIO) and is logged; the caller (UBI) then moves
 *     the data and has the block tested and, if it fails, marked bad.
 * Bad blocks: a marker in spare byte 0 of the block's first page, as U-Boot and
 * Yealink expect. There is no bad block table in flash and none is ever created.
 * No sub-page writes: a page is programmed once.
 *
 * The controller's own registers are left as U-Boot and Yealink set them (timing,
 * chip select, write protect pin); only the BCH engine is driven, for reading.
 *
 * Derived from Linux 3.6's drivers/mtd/nand/bcm_umi_nand.c, bcm_umi_bch.c and
 * nand_bcm_umi.[ch], Copyright 2004 - 2009 Broadcom Corporation, GPL version 2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/bitops.h>
#include <linux/platform_device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/partitions.h>

#define UMI_MMD_ICR		0x00c		/* bit 15: write protect pin released */
#define UMI_NAND_TCR		0x018		/* timing */
#define UMI_NAND_RCSR		0x01c		/* bit 1: chip ready */
#define  RCSR_RDY		0x02
#define UMI_BCH_N		0x040
#define UMI_BCH_K		0x044
#define UMI_BCH_T		0x048
#define UMI_BCH_CTRL		0x04c
#define  BCH_NB_CORR_SHIFT	20
#define  BCH_NB_CORR_MASK	0x00f00000
#define  BCH_UNCORR_ERR		0x00080000
#define  BCH_CORR_ERR		0x00040000
#define  BCH_RD_ECC_VALID	0x00020000
#define  BCH_PAUSE_ECC_DEC	0x00000010
#define  BCH_ECC_RD_EN		0x00000002
#define UMI_BCH_ERR_LOC		0x064		/* two 13-bit locations per word */
#define NAND_REG_CMD		0x800
#define NAND_REG_ADDR		0x804
#define NAND_REG_DATA		0x808

#define T48S_ECC_SIZE		512
#define T48S_ECC_BYTES		7
#define T48S_ECC_STRENGTH	4
#define T48S_OOB_PER_STEP	16
#define T48S_BCH_N		((T48S_ECC_SIZE + T48S_ECC_BYTES) * 8)
#define T48S_BCH_K		(T48S_BCH_N - T48S_ECC_STRENGTH * 13)

static void __iomem *umi;
static struct nand_controller t48s_nand_controller;
static struct nand_chip t48s_nand_chip;

static unsigned int blocked;	/* chip commands refused by layer 1 */
module_param(blocked, uint, 0444);
static unsigned int refused;	/* write requests refused by layers 2 and 3 */
module_param(refused, uint, 0444);
static unsigned int pages_written;
module_param(pages_written, uint, 0444);
static unsigned int blocks_erased;
module_param(blocks_erased, uint, 0444);
static unsigned int write_errors;	/* program or erase failed, or the read-back differed */
module_param(write_errors, uint, 0444);
static unsigned int selftest;	/* attempts outside our ranges made, and refused, at start-up */
module_param(selftest, uint, 0444);

static bool t48s_nand_testing;	/* the start-up self-test is running: no log lines */

/*
 * The only three places a byte leaves for the chip. latch_writes counts them,
 * which is how the self-test knows that a refusal sent nothing.
 */
static unsigned long latch_writes;

static void t48s_latch(u8 val, unsigned int reg)
{
	latch_writes++;
	writeb(val, umi + reg);
}

/* ---- what may be written: compiled in, nothing else ever ---- */

#define T48S_PAGE_SHIFT		11
#define T48S_PAGES_PER_BLOCK	64

/*
 * The last 60 blocks of Yealink's "app" partition. Its filesystem there fills
 * blocks 0 to 538 and has never touched these (they are erased in the stock
 * flash), and it is mounted read-only, so Yealink's firmware keeps working beside
 * what we keep here: UBI, with the phone's /data. Going back to stock is erasing
 * them. See docs/flash.md.
 */
static const struct mtd_partition t48s_nand_ours[] = {
	{ .name = "t48data", .offset = 0x06a80000, .size = 0x00780000 },
};

/* The only partitions a range may lie in. Never ubl, uboot, env, userenv, logo,
 * smkern, smrfs; and rfs, cfg and data stay Yealink's. Checked at start-up. */
static const struct { loff_t offset, size; } t48s_nand_reusable[] = {
	{ 0x015a0000, 0x00280000 },	/* kernel */
	{ 0x02720000, 0x04ae0000 },	/* app */
};

static bool t48s_nand_range_ok(loff_t ofs, loff_t len)
{
	int i;

	if (len <= 0)
		return false;
	for (i = 0; i < ARRAY_SIZE(t48s_nand_ours); i++)
		if (ofs >= t48s_nand_ours[i].offset &&
		    ofs + len <= t48s_nand_ours[i].offset + t48s_nand_ours[i].size)
			return true;
	return false;
}

static bool t48s_nand_page_ok(int page)
{
	return page >= 0 &&
	       t48s_nand_range_ok((loff_t)page << T48S_PAGE_SHIFT, 1 << T48S_PAGE_SHIFT);
}

static bool t48s_nand_block_ok(int page)
{
	return page >= 0 && !(page % T48S_PAGES_PER_BLOCK) &&
	       t48s_nand_range_ok((loff_t)page << T48S_PAGE_SHIFT,
				  T48S_PAGES_PER_BLOCK << T48S_PAGE_SHIFT);
}

static void t48s_nand_refuse(const char *what, long long where)
{
	refused++;
	if (refused <= 16 && !t48s_nand_testing)
		pr_warn("t48s_nand: %s at 0x%llx refused: not ours to write\n", what, where);
}

/* ---- layer 1: the only path from the kernel to the chip's command latch ---- */

static bool t48s_nand_cmd_ok(int cmd)
{
	switch (cmd) {
	case NAND_CMD_READ0:		/* 00h */
	case NAND_CMD_READSTART:	/* 30h */
	case NAND_CMD_RNDOUT:		/* 05h */
	case NAND_CMD_RNDOUTSTART:	/* E0h */
	case NAND_CMD_READID:		/* 90h */
	case NAND_CMD_PARAM:		/* ECh, the ONFI parameter page */
	case NAND_CMD_STATUS:		/* 70h */
		return true;
	}
	return false;
}

enum { GATE_PASS, GATE_DROP, GATE_PROG_ADDR, GATE_ERASE_ADDR, GATE_PROG_OPEN, GATE_ERASE_OPEN };
static int gate = GATE_PASS;
static u8 gate_addr[4];
static int gate_n;
static int announced_page = -1;	/* the page layer 2 is about to program or erase */

static void t48s_nand_block_cmd(int cmd)
{
	blocked++;
	if (blocked <= 16 && !t48s_nand_testing)
		pr_info("t48s_nand: chip command %02xh not sent\n", cmd & 0xff);
	gate = GATE_DROP;
}

static void t48s_nand_cmd_ctrl(struct nand_chip *chip, int cmd, unsigned int ctrl)
{
	int page, i;

	if (cmd == NAND_CMD_NONE)
		return;
	cmd &= 0xff;
	if (ctrl & NAND_CLE) {
		switch (cmd) {
		case NAND_CMD_SEQIN:	/* 80h: held until its four address bytes are in */
			gate = GATE_PROG_ADDR;
			gate_n = 0;
			return;
		case NAND_CMD_ERASE1:	/* 60h: held until its two address bytes are in */
			gate = GATE_ERASE_ADDR;
			gate_n = 0;
			return;
		case NAND_CMD_PAGEPROG:	/* 10h */
			if (gate != GATE_PROG_OPEN) {
				t48s_nand_block_cmd(cmd);
				return;
			}
			break;
		case NAND_CMD_ERASE2:	/* D0h */
			if (gate != GATE_ERASE_OPEN) {
				t48s_nand_block_cmd(cmd);
				return;
			}
			break;
		default:
			if (!t48s_nand_cmd_ok(cmd)) {
				t48s_nand_block_cmd(cmd);
				return;
			}
		}
		gate = GATE_PASS;
		t48s_latch(cmd, NAND_REG_CMD);
		return;
	}
	if (!(ctrl & NAND_ALE))
		return;		/* neither latch: data bytes go through write_buf only */
	switch (gate) {
	case GATE_PASS:
		t48s_latch(cmd, NAND_REG_ADDR);
		break;
	case GATE_PROG_ADDR:
		gate_addr[gate_n++] = cmd;
		if (gate_n < 4)
			break;
		/* column (0: a page; 2048: its spare bytes alone), then the page */
		page = gate_addr[2] | gate_addr[3] << 8;
		i = gate_addr[0] | gate_addr[1] << 8;
		if (!t48s_nand_page_ok(page) || page != announced_page || (i != 0 && i != 2048)) {
			t48s_nand_block_cmd(NAND_CMD_SEQIN);
			break;
		}
		t48s_latch(NAND_CMD_SEQIN, NAND_REG_CMD);
		for (i = 0; i < 4; i++)
			t48s_latch(gate_addr[i], NAND_REG_ADDR);
		gate = GATE_PROG_OPEN;
		break;
	case GATE_ERASE_ADDR:
		gate_addr[gate_n++] = cmd;
		if (gate_n < 2)
			break;
		page = gate_addr[0] | gate_addr[1] << 8;
		if (!t48s_nand_block_ok(page) || page != announced_page) {
			t48s_nand_block_cmd(NAND_CMD_ERASE1);
			break;
		}
		t48s_latch(NAND_CMD_ERASE1, NAND_REG_CMD);
		for (i = 0; i < 2; i++)
			t48s_latch(gate_addr[i], NAND_REG_ADDR);
		gate = GATE_ERASE_OPEN;
		break;
	default:
		/* dropped command, or address bytes where none belong: not sent */
		break;
	}
}

/* data bytes for the chip: only into a page program that layer 1 let through */
static void t48s_nand_write_buf(struct nand_chip *chip, const uint8_t *buf, int len)
{
	int i;

	if (gate != GATE_PROG_OPEN) {
		t48s_nand_refuse("data bytes", -1);
		return;
	}
	for (i = 0; i < len; i++)
		t48s_latch(buf[i], NAND_REG_DATA);
}

/* single bytes for the chip (set-features parameters): there are none to send */
static void t48s_nand_write_byte(struct nand_chip *chip, u8 byte)
{
	t48s_nand_refuse("a data byte", -1);
}

static int t48s_nand_dev_ready(struct nand_chip *chip)
{
	return readl(umi + UMI_NAND_RCSR) & RCSR_RDY;
}

/* ---- the ECC, in software: the code tools/nandcheck.py checks dumps with ----
 * Binary BCH over GF(2^13), primitive polynomial 0x201b, t = 4: 52 parity bits.
 * The 512 data bytes go in most significant bit first, then four zero bits; the
 * parity is stored big-endian in the low 52 bits of the 7 ECC bytes. */

#define T48S_BCH_BITS	52
#define T48S_BCH_MASK	((1ULL << T48S_BCH_BITS) - 1)
static u64 t48s_bch_table[256];
static u64 t48s_bch_low;	/* the generator polynomial without its top bit */

static u16 t48s_gf_mul(u16 a, u16 b)
{
	u16 r = 0;

	while (b) {
		if (b & 1)
			r ^= a;
		a <<= 1;
		if (a & 0x2000)
			a ^= 0x201b;
		b >>= 1;
	}
	return r;
}

static void t48s_bch_init(void)
{
	/* g(x) = product of the minimal polynomials of a^1, a^3, a^5, a^7, each of
	 * degree 13: product over the conjugates a^(i * 2^k) of (x - conjugate) */
	static const int roots[] = { 1, 3, 5, 7 };
	u64 g = 1;
	u16 alpha[4], c;
	int i, j, k, b;

	for (i = 0; i < 4; i++) {
		u16 poly[14] = { 1 };	/* poly[d]: coefficient of x^d, in GF(2^13) */
		u64 mp = 0, ng = 0;
		int deg = 0;

		alpha[i] = 1;
		for (j = 0; j < roots[i]; j++)
			alpha[i] = t48s_gf_mul(alpha[i], 2);
		c = alpha[i];
		for (k = 0; k < 13; k++) {
			/* poly *= (x + c) */
			for (j = deg + 1; j > 0; j--)
				poly[j] = poly[j - 1] ^ t48s_gf_mul(poly[j], c);
			poly[0] = t48s_gf_mul(poly[0], c);
			deg++;
			c = t48s_gf_mul(c, c);
		}
		for (j = 0; j <= 13; j++)
			if (poly[j] & 1)
				mp |= 1ULL << j;
		/* g *= mp, over GF(2) */
		for (j = 0; j <= 13; j++)
			if (mp >> j & 1)
				ng ^= g << j;
		g = ng;
	}
	t48s_bch_low = g & T48S_BCH_MASK;
	for (b = 0; b < 256; b++) {
		u64 r = (u64)b << (T48S_BCH_BITS - 8);

		for (j = 0; j < 8; j++)
			r = (r >> (T48S_BCH_BITS - 1) & 1) ?
				((r << 1) ^ t48s_bch_low) & T48S_BCH_MASK :
				(r << 1) & T48S_BCH_MASK;
		t48s_bch_table[b] = r;
	}
}

static void t48s_bch_ecc(const u8 *data, u8 *ecc)
{
	u64 r = 0;
	int i;

	for (i = 0; i < T48S_ECC_SIZE; i++)
		r = ((r << 8) & T48S_BCH_MASK) ^
		    t48s_bch_table[(r >> (T48S_BCH_BITS - 8)) ^ data[i]];
	for (i = 0; i < 4; i++)
		r = (r >> (T48S_BCH_BITS - 1) & 1) ?
			((r << 1) ^ t48s_bch_low) & T48S_BCH_MASK :
			(r << 1) & T48S_BCH_MASK;
	for (i = 0; i < T48S_ECC_BYTES; i++)
		ecc[i] = r >> (8 * (T48S_ECC_BYTES - 1 - i));
}

/* Known answers, so a mistake in the tables can never reach the flash: the ECC of
 * zeros, of a fixed pattern (worked out with tools/nandcheck.py), and the generator. */
static bool t48s_bch_selftest(void)
{
	static u8 d[T48S_ECC_SIZE];
	u8 e[T48S_ECC_BYTES];
	static const u8 zeros[T48S_ECC_BYTES] = { 0, 0, 0, 0, 0, 0, 0 };
	static const u8 ramp[T48S_ECC_BYTES] = { 0x01, 0x00, 0xfe, 0x27, 0x4b, 0x73, 0x07 };	/* from tools/nandcheck.py */
	int i;

	memset(d, 0, sizeof(d));
	t48s_bch_ecc(d, e);
	if (memcmp(e, zeros, sizeof(e)))
		return false;
	for (i = 0; i < T48S_ECC_SIZE; i++)
		d[i] = i * 7 + (i >> 8) * 13 + 1;
	t48s_bch_ecc(d, e);
	return !memcmp(e, ramp, sizeof(e)) && t48s_bch_low == 0x4523043ab86abULL;
}

/* ---- layer 2: writing, erasing and marking, each checking the page itself ---- */

static u8 t48s_nand_verify[2048 + 64];

/* raw read of a whole page with its spare bytes, the BCH engine left out of it */
static int t48s_nand_read_raw(struct nand_chip *chip, int page)
{
	writel(BCH_RD_ECC_VALID, umi + UMI_BCH_CTRL);
	return nand_read_page_op(chip, page, 0, t48s_nand_verify, sizeof(t48s_nand_verify));
}

static int t48s_nand_bits(const u8 *a, const u8 *b, int n)
{
	int i, bits = 0;

	for (i = 0; i < n; i++)
		bits += hweight8(a[i] ^ (b ? b[i] : 0xff));
	return bits;
}

static int t48s_nand_write_page(struct nand_chip *chip, const uint8_t *buf,
				int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	u8 *oob = chip->oob_poi;
	int step, bits, rc;

	if (!t48s_nand_page_ok(page)) {
		t48s_nand_refuse("page write", (long long)page << T48S_PAGE_SHIFT);
		return -EROFS;
	}
	/* never program a page that holds anything */
	rc = t48s_nand_read_raw(chip, page);
	if (rc)
		return rc;
	bits = t48s_nand_bits(t48s_nand_verify, NULL, sizeof(t48s_nand_verify));
	if (bits) {
		write_errors++;
		pr_err("t48s_nand: page %d is not erased (%d bits are 0); not written\n",
		       page, bits);
		return -EIO;
	}
	for (step = 0; step < 4; step++)
		t48s_bch_ecc(buf + step * T48S_ECC_SIZE,
			     oob + step * T48S_OOB_PER_STEP + T48S_OOB_PER_STEP - T48S_ECC_BYTES);

	announced_page = page;
	rc = nand_prog_page_begin_op(chip, page, 0, buf, mtd->writesize);
	if (!rc)
		rc = nand_write_data_op(chip, oob, mtd->oobsize, false);
	if (!rc)
		rc = nand_prog_page_end_op(chip);
	announced_page = -1;
	if (rc) {
		write_errors++;
		pr_err("t48s_nand: programming page %d failed (%d)\n", page, rc);
		return -EIO;
	}
	pages_written++;

	/* read back raw and compare everything, ECC bytes included */
	rc = t48s_nand_read_raw(chip, page);
	if (rc)
		return rc;
	bits = t48s_nand_bits(t48s_nand_verify, buf, mtd->writesize) +
	       t48s_nand_bits(t48s_nand_verify + mtd->writesize, oob, mtd->oobsize);
	if (bits) {
		write_errors++;
		pr_err("t48s_nand: page %d reads back with %d wrong bits after writing\n",
		       page, bits);
		return -EIO;
	}
	return 0;
}

static int t48s_nand_write_page_raw(struct nand_chip *chip, const uint8_t *buf,
				    int oob_required, int page)
{
	t48s_nand_refuse("raw page write", (long long)page << T48S_PAGE_SHIFT);
	return -EROFS;
}

/* Spare bytes alone: only ever a bad block marker (byte 0 zero, the rest untouched)
 * on the first page of one of our blocks. */
static int t48s_nand_write_oob(struct nand_chip *chip, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	int i, rc;

	if (!t48s_nand_block_ok(page)) {
		t48s_nand_refuse("spare byte write", (long long)page << T48S_PAGE_SHIFT);
		return -EROFS;
	}
	if (chip->oob_poi[0] != 0x00) {
		t48s_nand_refuse("spare byte write that is not a bad block marker",
				 (long long)page << T48S_PAGE_SHIFT);
		return -EROFS;
	}
	for (i = 1; i < mtd->oobsize; i++)
		if (chip->oob_poi[i] != 0xff) {
			t48s_nand_refuse("spare byte write that is not a bad block marker",
					 (long long)page << T48S_PAGE_SHIFT);
			return -EROFS;
		}
	pr_err("t48s_nand: MARKING BLOCK AT 0x%llx BAD\n", (long long)page << T48S_PAGE_SHIFT);
	announced_page = page;
	rc = nand_prog_page_begin_op(chip, page, mtd->writesize, chip->oob_poi, mtd->oobsize);
	if (!rc)
		rc = nand_prog_page_end_op(chip);
	announced_page = -1;
	return rc ? -EIO : 0;
}

static int t48s_nand_write_oob_raw(struct nand_chip *chip, int page)
{
	t48s_nand_refuse("raw spare byte write", (long long)page << T48S_PAGE_SHIFT);
	return -EROFS;
}

/*
 * The NAND layer has no erase hook any more: it sends the two erase commands
 * through the chip's command function. So that function is ours, in front of
 * the NAND layer's own (t48s_nand_core_cmdfunc, which turns a command into
 * latch cycles for layer 1). An erase is checked here and announced; a program
 * command is passed on only for the page the write code above announced.
 */
static void (*t48s_nand_core_cmdfunc)(struct nand_chip *, unsigned int, int, int);
static bool erase_passed;

static void t48s_nand_cmdfunc(struct nand_chip *chip, unsigned int command,
			      int column, int page)
{
	switch (command) {
	case NAND_CMD_ERASE1:
		erase_passed = false;
		if (column != -1 || !t48s_nand_block_ok(page)) {
			t48s_nand_refuse("erase", (long long)page << T48S_PAGE_SHIFT);
			return;
		}
		erase_passed = true;
		announced_page = page;
		break;
	case NAND_CMD_ERASE2:
		if (!erase_passed)
			return;
		t48s_nand_core_cmdfunc(chip, command, column, page);
		erase_passed = false;
		announced_page = -1;
		blocks_erased++;
		return;
	case NAND_CMD_SEQIN:
		if (page < 0 || page != announced_page) {
			t48s_nand_refuse("program command", (long long)page << T48S_PAGE_SHIFT);
			return;
		}
		break;
	}
	t48s_nand_core_cmdfunc(chip, command, column, page);
}

/* ---- layer 3: the MTD entry points refuse any address outside our ranges ---- */

static int (*t48s_nand_core_write_oob)(struct mtd_info *, loff_t, struct mtd_oob_ops *);
static int (*t48s_nand_core_erase)(struct mtd_info *, struct erase_info *);
static int (*t48s_nand_core_markbad)(struct mtd_info *, loff_t);

/* every write comes this way: the MTD core turns a plain write into this call */
static int t48s_mtd_write_oob(struct mtd_info *mtd, loff_t to, struct mtd_oob_ops *ops)
{
	/* whole pages with their free spare bytes (a restore of Yealink's pages puts
	 * its filesystem's tags back this way); never raw, never spare bytes alone */
	if (!ops->datbuf || ops->mode == MTD_OPS_RAW || !t48s_nand_range_ok(to, ops->len)) {
		t48s_nand_refuse("mtd write", to);
		return -EROFS;
	}
	return t48s_nand_core_write_oob(mtd, to, ops);
}

static int t48s_mtd_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	int rc;

	if (!t48s_nand_range_ok(instr->addr, instr->len)) {
		t48s_nand_refuse("mtd erase", instr->addr);
		return -EROFS;
	}
	rc = t48s_nand_core_erase(mtd, instr);
	if (rc)
		write_errors++;
	return rc;
}

static int t48s_mtd_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	if (!t48s_nand_range_ok(ofs & ~(loff_t)(mtd->erasesize - 1), mtd->erasesize)) {
		t48s_nand_refuse("mtd mark bad", ofs);
		return -EROFS;
	}
	write_errors++;
	return t48s_nand_core_markbad(mtd, ofs);
}

/* ---- the start-up self-test: every layer, asked to write what is not ours ---- */

/* one block in each partition that is not ours, and the blocks either side of ours */
static const loff_t t48s_nand_not_ours[] = {
	0x00000000,	/* ubl: the loader */
	0x00020000,	/* uboot */
	0x00120000,	/* env */
	0x00220000,	/* userenv */
	0x00320000,	/* logo */
	0x00420000,	/* smkern */
	0x006a0000,	/* smrfs */
	0x015a0000,	/* kernel */
	0x01820000,	/* rfs */
	0x02720000,	/* app, its first block */
	0x06a60000,	/* app, the block just below ours */
	0x07200000,	/* cfg, the block just above ours */
	0x07600000,	/* data */
	0x07fe0000,	/* the last block of the chip */
};

static u8 t48s_nand_testbuf[2048];

/* layer 1 alone, as if the layers above had a bug and announced this page */
static void t48s_nand_selftest_gate(struct nand_chip *chip, int page, int announce)
{
	const unsigned int cle = NAND_NCE | NAND_CLE | NAND_CTRL_CHANGE;
	const unsigned int ale = NAND_NCE | NAND_ALE;

	announced_page = announce;
	t48s_nand_cmd_ctrl(chip, NAND_CMD_SEQIN, cle);
	t48s_nand_cmd_ctrl(chip, 0, ale);
	t48s_nand_cmd_ctrl(chip, 0, ale);
	t48s_nand_cmd_ctrl(chip, page & 0xff, ale);
	t48s_nand_cmd_ctrl(chip, page >> 8, ale);
	if (gate != GATE_PROG_OPEN) {	/* refused, as it must be: go on to data and confirm */
		t48s_nand_write_buf(chip, t48s_nand_testbuf, 16);
		t48s_nand_cmd_ctrl(chip, NAND_CMD_PAGEPROG, cle);
	}
	t48s_nand_cmd_ctrl(chip, NAND_CMD_ERASE1, cle);
	t48s_nand_cmd_ctrl(chip, page & 0xff, ale);
	t48s_nand_cmd_ctrl(chip, page >> 8, ale);
	if (gate != GATE_ERASE_OPEN)
		t48s_nand_cmd_ctrl(chip, NAND_CMD_ERASE2, cle);
	announced_page = -1;
}

/*
 * Returns the number of attempts made, all refused with nothing sent to the
 * chip, or -1 if any attempt got through a layer. Runs before the flash is
 * registered, so nothing else is using the chip.
 */
static int t48s_nand_selftest(struct nand_chip *chip)
{
	static const u8 never[] = { NAND_CMD_RESET, NAND_CMD_RNDIN, NAND_CMD_SET_FEATURES,
				    NAND_CMD_CACHEDPROG, 0x23, 0x2a, 0x2c, 0xa0, 0xa1 };
	struct mtd_info *mtd = nand_to_mtd(chip);
	unsigned int was_blocked = blocked, was_refused = refused;
	unsigned long sent = latch_writes;
	struct mtd_oob_ops ops;
	struct erase_info ei;
	int i, page, n = 0, bad = 0;
	loff_t a;

#define T48S_MUST_REFUSE(expr) do { n++; if ((expr) != -EROFS) bad++; } while (0)
#define T48S_MUST_NOT_SEND(stmt) do { n++; stmt; } while (0)
	memset(t48s_nand_testbuf, 0x5a, sizeof(t48s_nand_testbuf));
	t48s_nand_testing = true;
	for (i = 0; i < ARRAY_SIZE(t48s_nand_not_ours); i++) {
		a = t48s_nand_not_ours[i];
		page = a >> T48S_PAGE_SHIFT;
		if (t48s_nand_range_ok(a, mtd->erasesize)) {
			bad++;		/* the list above is wrong, or the range check is */
			continue;
		}
		/* layer 3 */
		memset(&ei, 0, sizeof(ei));
		ei.addr = a;
		ei.len = mtd->erasesize;
		T48S_MUST_REFUSE(t48s_mtd_erase(mtd, &ei));
		memset(&ops, 0, sizeof(ops));
		ops.mode = MTD_OPS_PLACE_OOB;
		ops.datbuf = t48s_nand_testbuf;
		ops.len = mtd->writesize;
		T48S_MUST_REFUSE(t48s_mtd_write_oob(mtd, a, &ops));
		T48S_MUST_REFUSE(t48s_mtd_block_markbad(mtd, a));
		/* layer 2 */
		T48S_MUST_REFUSE(t48s_nand_write_page(chip, t48s_nand_testbuf, 0, page));
		T48S_MUST_REFUSE(t48s_nand_write_page_raw(chip, t48s_nand_testbuf, 0, page));
		memset(chip->oob_poi, 0xff, mtd->oobsize);
		chip->oob_poi[0] = 0;	/* a bad block marker, well formed */
		T48S_MUST_REFUSE(t48s_nand_write_oob(chip, page));
		T48S_MUST_REFUSE(t48s_nand_write_oob_raw(chip, page));
		memset(chip->oob_poi, 0xff, mtd->oobsize);
		T48S_MUST_NOT_SEND(t48s_nand_cmdfunc(chip, NAND_CMD_ERASE1, -1, page);
				   t48s_nand_cmdfunc(chip, NAND_CMD_ERASE2, -1, -1));
		T48S_MUST_NOT_SEND(t48s_nand_cmdfunc(chip, NAND_CMD_SEQIN, 0, page);
				   t48s_nand_write_buf(chip, t48s_nand_testbuf, 16);
				   t48s_nand_cmdfunc(chip, NAND_CMD_PAGEPROG, -1, -1));
		/* layer 1 */
		T48S_MUST_NOT_SEND(t48s_nand_selftest_gate(chip, page, page));
	}
	/* across each end of our range */
	a = t48s_nand_ours[0].offset;
	memset(&ei, 0, sizeof(ei));
	ei.addr = a - mtd->erasesize;
	ei.len = 2 * mtd->erasesize;
	T48S_MUST_REFUSE(t48s_mtd_erase(mtd, &ei));
	ei.addr = a + t48s_nand_ours[0].size - mtd->erasesize;
	T48S_MUST_REFUSE(t48s_mtd_erase(mtd, &ei));
	memset(&ops, 0, sizeof(ops));
	ops.datbuf = t48s_nand_testbuf;
	ops.len = mtd->writesize;
	T48S_MUST_REFUSE(t48s_mtd_write_oob(mtd, a + t48s_nand_ours[0].size - 1024, &ops));
	/* inside our range, the kinds of write that do not exist */
	ops.mode = MTD_OPS_RAW;
	T48S_MUST_REFUSE(t48s_mtd_write_oob(mtd, a, &ops));
	ops.mode = MTD_OPS_PLACE_OOB;
	ops.datbuf = NULL;
	ops.oobbuf = t48s_nand_testbuf;
	ops.ooblen = 1;
	T48S_MUST_REFUSE(t48s_mtd_write_oob(mtd, a, &ops));
	page = a >> T48S_PAGE_SHIFT;
	T48S_MUST_REFUSE(t48s_nand_write_page_raw(chip, t48s_nand_testbuf, 0, page));
	T48S_MUST_REFUSE(t48s_nand_write_oob_raw(chip, page));
	memset(chip->oob_poi, 0xff, mtd->oobsize);
	chip->oob_poi[1] = 0;	/* spare bytes that are not a bad block marker */
	T48S_MUST_REFUSE(t48s_nand_write_oob(chip, page));
	chip->oob_poi[1] = 0xff;
	chip->oob_poi[0] = 0;	/* a marker, but not on a block's first page */
	T48S_MUST_REFUSE(t48s_nand_write_oob(chip, page + 1));
	memset(chip->oob_poi, 0xff, mtd->oobsize);
	/* ... a program command for a page nobody announced, at layers 2 and 1; an
	 * erase that does not start on a block; a program at a column inside a page */
	T48S_MUST_NOT_SEND(t48s_nand_cmdfunc(chip, NAND_CMD_SEQIN, 0, page));
	T48S_MUST_NOT_SEND(t48s_nand_selftest_gate(chip, page, -1));
	T48S_MUST_NOT_SEND(t48s_nand_cmdfunc(chip, NAND_CMD_ERASE1, -1, page + 1);
			   t48s_nand_cmdfunc(chip, NAND_CMD_ERASE2, -1, -1));
	T48S_MUST_NOT_SEND(t48s_nand_selftest_gate(chip, page + 1, page));
	T48S_MUST_NOT_SEND(announced_page = page;
			   t48s_nand_cmd_ctrl(chip, NAND_CMD_SEQIN, NAND_NCE | NAND_CLE);
			   t48s_nand_cmd_ctrl(chip, 100, NAND_NCE | NAND_ALE);
			   t48s_nand_cmd_ctrl(chip, 0, NAND_NCE | NAND_ALE);
			   t48s_nand_cmd_ctrl(chip, page & 0xff, NAND_NCE | NAND_ALE);
			   t48s_nand_cmd_ctrl(chip, page >> 8, NAND_NCE | NAND_ALE);
			   announced_page = -1);
	/* ... and the commands that are never sent anywhere, and stray data */
	for (i = 0; i < ARRAY_SIZE(never); i++)
		T48S_MUST_NOT_SEND(t48s_nand_cmd_ctrl(chip, never[i], NAND_NCE | NAND_CLE);
				   t48s_nand_cmd_ctrl(chip, 0, NAND_NCE | NAND_ALE));
	T48S_MUST_NOT_SEND(t48s_nand_cmd_ctrl(chip, NAND_CMD_PAGEPROG, NAND_NCE | NAND_CLE));
	T48S_MUST_NOT_SEND(t48s_nand_cmd_ctrl(chip, NAND_CMD_ERASE2, NAND_NCE | NAND_CLE));
	T48S_MUST_NOT_SEND(t48s_nand_write_buf(chip, t48s_nand_testbuf, 16));
	T48S_MUST_NOT_SEND(t48s_nand_write_byte(chip, 0));
#undef T48S_MUST_REFUSE
#undef T48S_MUST_NOT_SEND

	if (latch_writes != sent)
		bad++;
	if (gate == GATE_PROG_OPEN || gate == GATE_ERASE_OPEN || announced_page != -1 || erase_passed)
		bad++;
	t48s_nand_testing = false;
	gate = GATE_DROP;	/* until the next command that reads */
	blocked = was_blocked;
	refused = was_refused;
	if (bad) {
		pr_err("t48s_nand: SELF-TEST FAILED: a write outside our range was not refused (%lu bytes reached the chip's latches)\n",
		       latch_writes - sent);
		return -1;
	}
	return n;
}

/* ---- reading ---- */

/* Flip one bit the BCH engine located; positions beyond the data are in the ECC bytes. */
static void t48s_nand_flip(uint8_t *data, unsigned int loc)
{
	unsigned int bit = loc & 7;
	unsigned int byte = ((loc >> 5) & 0xff) * 4 + (3 - ((loc >> 3) & 3));

	if (byte < T48S_ECC_SIZE)
		data[byte] ^= 1 << bit;
}

/* Returns the number of bits corrected in this 512-byte step, or -1. */
static int t48s_nand_correct(uint8_t *data, const uint8_t *ecc)
{
	u32 st;
	int i, n, tries = 1000000;

	do {
		st = readl(umi + UMI_BCH_CTRL);
	} while (!(st & BCH_RD_ECC_VALID) && --tries);
	if (!tries)
		return -1;
	if (st & BCH_UNCORR_ERR) {
		/* an erased step has no ECC: nothing to check it against */
		for (i = 0; i < T48S_ECC_BYTES; i++)
			if (ecc[i] != 0xff)
				return -1;
		return 0;
	}
	if (!(st & BCH_CORR_ERR))
		return 0;
	n = (st & BCH_NB_CORR_MASK) >> BCH_NB_CORR_SHIFT;
	for (i = 0; i < n; i++)
		t48s_nand_flip(data, (readl(umi + UMI_BCH_ERR_LOC + (i / 2) * 4)
				      >> ((i % 2) * 16)) & 0x1fff);
	return n;
}

static int t48s_nand_read_page(struct nand_chip *chip, uint8_t *buf,
			       int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	unsigned int max_bitflips = 0;
	int step, i, stat, rc;

	rc = nand_read_page_op(chip, page, 0, NULL, 0);
	if (rc)
		return rc;
	for (step = 0; step < chip->ecc.steps; step++) {
		uint8_t *data = buf + step * T48S_ECC_SIZE;
		uint8_t *oob = chip->oob_poi + step * T48S_OOB_PER_STEP;

		if (step) {
			rc = nand_change_read_column_op(chip, step * T48S_ECC_SIZE,
							NULL, 0, false);
			if (rc)
				return rc;
		}
		/* reset the engine, then let it follow the data */
		writel(BCH_RD_ECC_VALID, umi + UMI_BCH_CTRL);
		writel(BCH_ECC_RD_EN, umi + UMI_BCH_CTRL);
		ioread8_rep(umi + NAND_REG_DATA, data, T48S_ECC_SIZE);
		writel(BCH_ECC_RD_EN | BCH_PAUSE_ECC_DEC, umi + UMI_BCH_CTRL);
		/* this step's spare bytes: 9 the engine must not see, then the ECC */
		rc = nand_change_read_column_op(chip, mtd->writesize + step * T48S_OOB_PER_STEP,
						NULL, 0, false);
		if (rc)
			return rc;
		for (i = 0; i < T48S_OOB_PER_STEP - T48S_ECC_BYTES; i++)
			oob[i] = readb(umi + NAND_REG_DATA);
		writel(BCH_ECC_RD_EN, umi + UMI_BCH_CTRL);
		for (; i < T48S_OOB_PER_STEP; i++)
			oob[i] = readb(umi + NAND_REG_DATA);

		stat = t48s_nand_correct(data, oob + T48S_OOB_PER_STEP - T48S_ECC_BYTES);
		if (stat < 0) {
			mtd->ecc_stats.failed++;
		} else {
			mtd->ecc_stats.corrected += stat;
			max_bitflips = max_t(unsigned int, max_bitflips, stat);
		}
	}
	/* leave the engine out of whatever is read next */
	writel(BCH_RD_ECC_VALID, umi + UMI_BCH_CTRL);
	return max_bitflips;
}

/* a whole page with its spare bytes exactly as stored, the BCH engine left out of it */
static int t48s_nand_read_page_raw(struct nand_chip *chip, uint8_t *buf,
				   int oob_required, int page)
{
	writel(BCH_RD_ECC_VALID, umi + UMI_BCH_CTRL);
	return nand_read_page_raw(chip, buf, oob_required, page);
}

static int t48s_ooblayout_ecc(struct mtd_info *mtd, int section,
			      struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;
	region->offset = section * T48S_OOB_PER_STEP + T48S_OOB_PER_STEP - T48S_ECC_BYTES;
	region->length = T48S_ECC_BYTES;
	return 0;
}

static int t48s_ooblayout_free(struct mtd_info *mtd, int section,
			       struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;
	/* byte 0 of the first group is the bad block marker */
	region->offset = section * T48S_OOB_PER_STEP + (section ? 0 : 1);
	region->length = T48S_OOB_PER_STEP - T48S_ECC_BYTES - (section ? 0 : 1);
	return 0;
}

static const struct mtd_ooblayout_ops t48s_ooblayout_ops = {
	.ecc = t48s_ooblayout_ecc,
	.free = t48s_ooblayout_free,
};

static int t48s_nand_attach_chip(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);

	if (mtd->writesize != 2048 || mtd->oobsize != 64 || mtd->erasesize != 0x20000 ||
	    mtd->size != 0x08000000) {
		pr_err("t48s_nand: not the chip this driver knows: page %u, spare %u, block %u, size %llu\n",
		       mtd->writesize, mtd->oobsize, mtd->erasesize,
		       (unsigned long long)mtd->size);
		return -ENODEV;
	}
	chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;
	chip->ecc.placement = NAND_ECC_PLACEMENT_OOB;
	chip->ecc.size = T48S_ECC_SIZE;
	chip->ecc.bytes = T48S_ECC_BYTES;
	chip->ecc.strength = T48S_ECC_STRENGTH;
	chip->ecc.read_page = t48s_nand_read_page;
	chip->ecc.read_page_raw = t48s_nand_read_page_raw;
	chip->ecc.write_page = t48s_nand_write_page;
	chip->ecc.write_page_raw = t48s_nand_write_page_raw;
	chip->ecc.write_oob = t48s_nand_write_oob;
	chip->ecc.write_oob_raw = t48s_nand_write_oob_raw;
	mtd_set_ooblayout(mtd, &t48s_ooblayout_ops);
	return 0;
}

static const struct nand_controller_ops t48s_nand_controller_ops = {
	.attach_chip = t48s_nand_attach_chip,
};

/* Yealink's layout (its mtdparts). mtd9 is a zero-size "boot" there; here it is the
 * whole chip, which keeps the other numbers the same under every kernel. */
#define T48S_PART(nm, off, sz) \
	{ .name = nm, .offset = off, .size = sz, .mask_flags = MTD_WRITEABLE }
static const struct mtd_partition t48s_nand_parts[] = {
	T48S_PART("ubl",     0x00000000, 0x00020000),
	T48S_PART("uboot",   0x00020000, 0x00100000),
	T48S_PART("env",     0x00120000, 0x00100000),
	T48S_PART("userenv", 0x00220000, 0x00100000),
	T48S_PART("logo",    0x00320000, 0x00100000),
	T48S_PART("smkern",  0x00420000, 0x00280000),
	T48S_PART("smrfs",   0x006a0000, 0x00f00000),
	T48S_PART("kernel",  0x015a0000, 0x00280000),
	T48S_PART("rfs",     0x01820000, 0x00f00000),
	T48S_PART("whole",   0x00000000, 0x08000000),
	T48S_PART("app",     0x02720000, 0x04ae0000),
	T48S_PART("cfg",     0x07200000, 0x00400000),
	T48S_PART("data",    0x07600000, 0x00a00000),
	/* ours, and the only writeable ones: t48s_nand_ours[] is added after these */
};

static int t48s_nand_probe(struct platform_device *pdev)
{
	static struct mtd_partition parts[ARRAY_SIZE(t48s_nand_parts) + ARRAY_SIZE(t48s_nand_ours)];
	struct nand_chip *chip = &t48s_nand_chip;
	struct mtd_info *mtd = nand_to_mtd(chip);
	loff_t ofs;
	int err, bad = 0, i, j;

	t48s_bch_init();
	if (!t48s_bch_selftest()) {
		pr_err("t48s_nand: the software ECC fails its self-test; no flash\n");
		return -EINVAL;
	}
	/* every range we may write must lie whole blocks inside kernel or app */
	for (i = 0; i < ARRAY_SIZE(t48s_nand_ours); i++) {
		const struct mtd_partition *o = &t48s_nand_ours[i];

		for (j = 0; j < ARRAY_SIZE(t48s_nand_reusable); j++)
			if (o->offset >= t48s_nand_reusable[j].offset &&
			    o->offset + o->size <= t48s_nand_reusable[j].offset +
						   t48s_nand_reusable[j].size)
				break;
		if (j == ARRAY_SIZE(t48s_nand_reusable) || o->mask_flags ||
		    (o->offset & 0x1ffff) || (o->size & 0x1ffff) || !o->size) {
			pr_err("t48s_nand: writeable range %s is not inside kernel or app; no flash\n",
			       o->name);
			return -EINVAL;
		}
	}

	umi = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(umi))
		return PTR_ERR(umi);
	pr_info("t48s_nand: controller as found: timing %08x, control %08x (write protect pin %s), status %08x, BCH n=%u k=%u t=%u\n",
		readl(umi + UMI_NAND_TCR), readl(umi + UMI_MMD_ICR),
		(readl(umi + UMI_MMD_ICR) & 0x8000) ? "released" : "held",
		readl(umi + UMI_NAND_RCSR), readl(umi + UMI_BCH_N),
		readl(umi + UMI_BCH_K), readl(umi + UMI_BCH_T));
	if (readl(umi + UMI_BCH_N) != T48S_BCH_N || readl(umi + UMI_BCH_K) != T48S_BCH_K ||
	    readl(umi + UMI_BCH_T) != T48S_ECC_STRENGTH) {
		pr_info("t48s_nand: setting BCH n=%u k=%u t=%u\n",
			T48S_BCH_N, T48S_BCH_K, T48S_ECC_STRENGTH);
		writel(BCH_RD_ECC_VALID, umi + UMI_BCH_CTRL);
		writel(T48S_BCH_N, umi + UMI_BCH_N);
		writel(T48S_ECC_STRENGTH, umi + UMI_BCH_T);
		writel(T48S_BCH_K, umi + UMI_BCH_K);
	}

	nand_controller_init(&t48s_nand_controller);
	t48s_nand_controller.ops = &t48s_nand_controller_ops;
	chip->controller = &t48s_nand_controller;
	mtd->dev.parent = &pdev->dev;
	mtd->owner = THIS_MODULE;
	chip->legacy.IO_ADDR_R = umi + NAND_REG_DATA;
	chip->legacy.IO_ADDR_W = NULL;		/* the NAND layer's own writers have no address */
	chip->legacy.cmd_ctrl = t48s_nand_cmd_ctrl;
	chip->legacy.dev_ready = t48s_nand_dev_ready;
	chip->legacy.write_buf = t48s_nand_write_buf;
	chip->legacy.write_byte = t48s_nand_write_byte;
	chip->legacy.chip_delay = 0;
	chip->options = NAND_NO_SUBPAGE_WRITE;	/* a page is programmed once */
	chip->bbt_options = 0;			/* bad block table in RAM only, never in flash */

	err = nand_scan(chip, 1);
	if (err) {
		pr_err("t48s_nand: no chip, or not the one we know (%d)\n", err);
		return err;
	}

	/* layer 2's command hook goes in front of the NAND layer's */
	t48s_nand_core_cmdfunc = chip->legacy.cmdfunc;
	chip->legacy.cmdfunc = t48s_nand_cmdfunc;
	/* layer 3: the entry points check the address before the NAND layer sees it */
	t48s_nand_core_write_oob = mtd->_write_oob;
	t48s_nand_core_erase = mtd->_erase;
	t48s_nand_core_markbad = mtd->_block_markbad;
	mtd->_write_oob = t48s_mtd_write_oob;
	mtd->_erase = t48s_mtd_erase;
	mtd->_block_markbad = t48s_mtd_block_markbad;
	mtd->_panic_write = NULL;
	mtd->_lock = NULL;
	mtd->_unlock = NULL;
	if (!t48s_nand_core_cmdfunc || !t48s_nand_core_write_oob || !t48s_nand_core_erase ||
	    !t48s_nand_core_markbad ||
	    chip->legacy.cmd_ctrl != t48s_nand_cmd_ctrl ||
	    chip->legacy.write_buf != t48s_nand_write_buf ||
	    chip->legacy.write_byte != t48s_nand_write_byte ||
	    chip->legacy.IO_ADDR_W ||
	    chip->ecc.write_page != t48s_nand_write_page ||
	    chip->ecc.write_page_raw != t48s_nand_write_page_raw ||
	    chip->ecc.write_subpage ||
	    chip->ecc.write_oob != t48s_nand_write_oob ||
	    chip->ecc.write_oob_raw != t48s_nand_write_oob_raw ||
	    chip->controller->ops->exec_op ||
	    chip->legacy.block_markbad ||
	    mtd->_write || mtd->_writev || mtd->_write_user_prot_reg ||
	    mtd->_lock_user_prot_reg || mtd->_erase_user_prot_reg ||
	    (chip->options & (NAND_ROW_ADDR_3 | NAND_BUSWIDTH_16)) ||
	    (chip->bbt_options & NAND_BBT_USE_FLASH) || mtd->subpage_sft) {
		pr_err("t48s_nand: a write hook is not ours; not registering the flash\n");
		nand_cleanup(chip);
		return -EPERM;
	}
	err = t48s_nand_selftest(chip);
	if (err < 0) {
		nand_cleanup(chip);
		return -EPERM;
	}
	selftest = err;
	pr_info("t48s_nand: self-test: %d attempts to erase or program outside our range, at all three layers: every one refused, nothing sent to the chip\n",
		selftest);

	/* Yealink's partitions, all read-only, then ours */
	memcpy(parts, t48s_nand_parts, sizeof(t48s_nand_parts));
	memcpy(parts + ARRAY_SIZE(t48s_nand_parts), t48s_nand_ours, sizeof(t48s_nand_ours));
	for (i = 0; i < ARRAY_SIZE(t48s_nand_parts); i++)
		if (!(parts[i].mask_flags & MTD_WRITEABLE)) {
			nand_cleanup(chip);
			return -EPERM;
		}
	mtd->name = "t48s-nand";
	err = mtd_device_register(mtd, parts, ARRAY_SIZE(parts));
	if (err) {
		nand_cleanup(chip);
		return err;
	}
	for (ofs = 0; ofs < mtd->size; ofs += mtd->erasesize)
		if (mtd_block_isbad(mtd, ofs))
			bad++;
	pr_info("t48s_nand: %d bad blocks, %u chip commands blocked while scanning\n",
		bad, blocked);
	for (i = 0; i < ARRAY_SIZE(t48s_nand_ours); i++)
		pr_info("t48s_nand: writeable: %s, 0x%08llx to 0x%08llx; everything else is read-only\n",
			t48s_nand_ours[i].name, (unsigned long long)t48s_nand_ours[i].offset,
			(unsigned long long)(t48s_nand_ours[i].offset + t48s_nand_ours[i].size));
	return 0;
}

static const struct of_device_id t48s_nand_of_match[] = {
	{ .compatible = "brcm,bcm11107-umi-nand" },
	{ }
};

static struct platform_driver t48s_nand_driver = {
	.probe = t48s_nand_probe,
	.driver = {
		.name = "t48s_nand",
		.of_match_table = t48s_nand_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(t48s_nand_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Yealink T48S NAND flash: read everywhere, write only our own ranges");
