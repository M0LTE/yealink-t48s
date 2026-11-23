// SPDX-License-Identifier: GPL-2.0
/*
 * Yealink T48S: the NAND flash, READ-ONLY (kernel6, issue #51).
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
 * This is the reading half of kernel/drivers/t48s_nand.c (Linux 3.6), ported to
 * the current raw NAND layer. THERE IS NO CODE HERE THAT PROGRAMS OR ERASES, and
 * three layers each make sure of it on their own:
 *
 *  1. Chip commands pass through one function, t48s_nand_cmd_ctrl(), which sends
 *     only the commands that read (a fixed list). Program, erase, reset, lock,
 *     set-features and anything unknown are dropped, with the address bytes
 *     that follow them, and counted in /sys/module/t48s_nand/parameters/blocked.
 *     Nothing in this file writes to the chip's data latch.
 *  2. The write hooks of the NAND layer (page, raw page, spare bytes) only
 *     return -EROFS.
 *  3. The MTD device is not writeable: its flag is cleared, its write, erase and
 *     mark-bad entry points are replaced by ones that return -EROFS, and every
 *     partition is registered read-only.
 *
 * Bad blocks: the marker in spare byte 0 of a block's first page is read into a
 * table in RAM at start-up. Nothing is written to the flash for it.
 *
 * The controller's own registers are left as U-Boot and Yealink set them (timing,
 * chip select, write protect pin); only the BCH engine is driven.
 *
 * Derived from Linux 3.6's drivers/mtd/nand/bcm_umi_nand.c, bcm_umi_bch.c and
 * nand_bcm_umi.[ch], Copyright 2004 - 2009 Broadcom Corporation, GPL version 2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
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

static unsigned int blocked;	/* chip commands not sent (layer 1) */
module_param(blocked, uint, 0444);
static unsigned int refused;	/* write requests refused (layers 2 and 3) */
module_param(refused, uint, 0444);

static int t48s_nand_refuse(const char *what)
{
	refused++;
	if (refused <= 16)
		pr_warn("t48s_nand: %s refused: this kernel only reads the flash\n", what);
	return -EROFS;
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

static bool gate_open = true;	/* false after a dropped command: its address bytes are dropped too */

static void t48s_nand_cmd_ctrl(struct nand_chip *chip, int cmd, unsigned int ctrl)
{
	if (cmd == NAND_CMD_NONE)
		return;
	cmd &= 0xff;
	if (ctrl & NAND_CLE) {
		if (!t48s_nand_cmd_ok(cmd)) {
			blocked++;
			if (blocked <= 16)
				pr_info("t48s_nand: chip command %02xh not sent\n", cmd);
			gate_open = false;
			return;
		}
		gate_open = true;
		writeb(cmd, umi + NAND_REG_CMD);
		return;
	}
	if ((ctrl & NAND_ALE) && gate_open)
		writeb(cmd, umi + NAND_REG_ADDR);
	/* neither latch would be data for the chip: there is none to send, ever */
}

static void t48s_nand_write_buf(struct nand_chip *chip, const uint8_t *buf, int len)
{
	t48s_nand_refuse("data bytes for the chip");
}

static int t48s_nand_dev_ready(struct nand_chip *chip)
{
	return readl(umi + UMI_NAND_RCSR) & RCSR_RDY;
}

/* ---- layer 2: the NAND layer's write hooks ---- */

static int t48s_nand_write_page(struct nand_chip *chip, const uint8_t *buf,
				int oob_required, int page)
{
	return t48s_nand_refuse("page write");
}

static int t48s_nand_write_oob(struct nand_chip *chip, int page)
{
	return t48s_nand_refuse("spare byte write");
}

/* ---- layer 3: the MTD entry points ---- */

static int t48s_mtd_write(struct mtd_info *mtd, loff_t to, size_t len,
			  size_t *retlen, const u_char *buf)
{
	return t48s_nand_refuse("mtd write");
}

static int t48s_mtd_write_oob(struct mtd_info *mtd, loff_t to, struct mtd_oob_ops *ops)
{
	return t48s_nand_refuse("mtd write with spare bytes");
}

static int t48s_mtd_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	return t48s_nand_refuse("mtd erase");
}

static int t48s_mtd_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	return t48s_nand_refuse("mtd mark bad");
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
	chip->ecc.write_page_raw = t48s_nand_write_page;
	chip->ecc.write_oob = t48s_nand_write_oob;
	chip->ecc.write_oob_raw = t48s_nand_write_oob;
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
};

static int t48s_nand_probe(struct platform_device *pdev)
{
	struct nand_chip *chip = &t48s_nand_chip;
	struct mtd_info *mtd = nand_to_mtd(chip);
	loff_t ofs;
	int err, bad = 0, i;

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
	chip->legacy.IO_ADDR_W = NULL;		/* nothing may reach the data latch */
	chip->legacy.cmd_ctrl = t48s_nand_cmd_ctrl;
	chip->legacy.dev_ready = t48s_nand_dev_ready;
	chip->legacy.write_buf = t48s_nand_write_buf;
	chip->legacy.chip_delay = 0;
	chip->options = NAND_NO_SUBPAGE_WRITE;
	chip->bbt_options = 0;			/* bad block table in RAM only, never in flash */

	err = nand_scan(chip, 1);
	if (err) {
		pr_err("t48s_nand: no chip, or not the one we know (%d)\n", err);
		return err;
	}

	/* layer 3 */
	mtd->flags &= ~MTD_WRITEABLE;
	mtd->_write = t48s_mtd_write;
	mtd->_write_oob = t48s_mtd_write_oob;
	mtd->_panic_write = NULL;
	mtd->_erase = t48s_mtd_erase;
	mtd->_block_markbad = t48s_mtd_block_markbad;
	if (chip->legacy.write_buf != t48s_nand_write_buf ||
	    chip->legacy.cmd_ctrl != t48s_nand_cmd_ctrl ||
	    chip->ecc.write_page != t48s_nand_write_page ||
	    chip->ecc.write_page_raw != t48s_nand_write_page ||
	    chip->ecc.write_oob != t48s_nand_write_oob ||
	    chip->ecc.write_oob_raw != t48s_nand_write_oob ||
	    chip->controller->ops->exec_op ||
	    (chip->bbt_options & NAND_BBT_USE_FLASH)) {
		pr_err("t48s_nand: a write hook is not ours; not registering the flash\n");
		nand_cleanup(chip);
		return -EPERM;
	}
	for (i = 0; i < ARRAY_SIZE(t48s_nand_parts); i++)
		if (!(t48s_nand_parts[i].mask_flags & MTD_WRITEABLE)) {
			nand_cleanup(chip);
			return -EPERM;
		}

	mtd->name = "t48s-nand";
	err = mtd_device_register(mtd, t48s_nand_parts, ARRAY_SIZE(t48s_nand_parts));
	if (err) {
		nand_cleanup(chip);
		return err;
	}
	for (ofs = 0; ofs < mtd->size; ofs += mtd->erasesize)
		if (mtd_block_isbad(mtd, ofs))
			bad++;
	pr_info("t48s_nand: read-only; %d bad blocks, %u chip commands not sent while scanning\n",
		bad, blocked);
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
MODULE_DESCRIPTION("Yealink T48S NAND flash, read-only");
