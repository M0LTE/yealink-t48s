// SPDX-License-Identifier: GPL-2.0
/*
 * Yealink T48S: read-only access to the NAND flash (issue #5).
 *
 * The SoC's UMI block drives a Winbond 128 MB chip (ID ef f1, 2048-byte pages, 64
 * spare bytes, 128 KB blocks) and has a hardware BCH engine. This is mainline 3.6's
 * bcm_umi_nand cut down to reading, with the error correction set the way the phone's
 * U-Boot and Yealink's kernel use it, which is not mainline's default:
 *
 *   4 correctable bits per 512 data bytes, 7 ECC bytes each (mainline: 8 bits, 13 bytes)
 *   spare area, four 16-byte groups, one per 512 bytes of data:
 *     byte 0      bad block marker in the first group, free in the others
 *     bytes 1-8   free (Yealink's yaffs2 keeps its tags here), not covered by the ECC
 *     bytes 9-15  ECC
 *
 * THIS DRIVER CANNOT CHANGE THE FLASH. Three independent layers, any one is enough:
 *
 *  1. Chip commands pass through one function, t48s_nand_cmd_ctrl(), which sends only
 *     the commands in t48s_nand_cmd_ok(): read page, random data output, read ID,
 *     read parameter page, read status. Program (80h/10h/85h/15h), erase (60h/D0h),
 *     lock (2Ah/2Ch/23h), reset (FFh) and anything else are dropped and counted
 *     (/sys/module/t48s_nand/parameters/blocked), together with the address bytes
 *     that follow them.
 *  2. Every write hook of the NAND layer (write_buf, write_page, the ECC write
 *     methods, erase_cmd, block_markbad) is a stub here that sends nothing and
 *     returns -EROFS where it can. There is no bad block table in flash, so the
 *     scan at boot only reads.
 *  3. MTD_WRITEABLE is cleared on the whole device before it is registered and
 *     masked on every partition, so the MTD core answers -EROFS to write, erase
 *     and mark-bad, and /dev/mtdN cannot be opened for writing. The write and erase
 *     entry points of the device are removed as well.
 *
 * The controller's own registers are left as U-Boot and Yealink set them (timing,
 * chip select, write protect pin); only the BCH engine is driven, and its three
 * parameters are put back if they are not what the flash contents need.
 *
 * Derived from drivers/mtd/nand/bcm_umi_nand.c, bcm_umi_bch.c and nand_bcm_umi.[ch],
 * Copyright 2004 - 2009 Broadcom Corporation, licensed under the GPL version 2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>

#ifdef CONFIG_MTD_NAND_BCM_UMI
#error "build t48s_nand without mainline's bcm_umi_nand: that one can write"
#endif

#define T48S_UMI_PHYS		0x80001000	/* UMI block; the NAND registers are at +0x800 */
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
static struct mtd_info *t48s_mtd;

static unsigned int blocked;	/* chip commands refused by layer 1 */
module_param(blocked, uint, 0444);
static unsigned int refused;	/* calls into the write stubs of layer 2 */
module_param(refused, uint, 0444);

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

static void t48s_nand_cmd_ctrl(struct mtd_info *mtd, int cmd, unsigned int ctrl)
{
	static bool dropping;

	if (cmd == NAND_CMD_NONE)
		return;
	if (ctrl & NAND_CLE) {
		dropping = !t48s_nand_cmd_ok(cmd & 0xff);
		if (dropping) {
			blocked++;
			if (blocked <= 8)
				pr_info("t48s_nand: read-only, chip command %02xh not sent\n",
					cmd & 0xff);
			return;
		}
		writeb(cmd, umi + NAND_REG_CMD);
	} else if (ctrl & NAND_ALE) {
		if (!dropping)
			writeb(cmd, umi + NAND_REG_ADDR);
	}
	/* neither latch: a data byte for the chip. Nothing is ever sent that way. */
}

/* ---- layer 2: every write hook is a stub ---- */

static void t48s_nand_refuse(const char *what)
{
	refused++;
	if (refused <= 8)
		pr_warn("t48s_nand: read-only, %s refused\n", what);
}

static void t48s_nand_write_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	t48s_nand_refuse("write_buf");
}

static void t48s_nand_ecc_write_page(struct mtd_info *mtd, struct nand_chip *chip,
				     const uint8_t *buf, int oob_required)
{
	t48s_nand_refuse("ecc.write_page");
}

static int t48s_nand_ecc_write_oob(struct mtd_info *mtd, struct nand_chip *chip, int page)
{
	t48s_nand_refuse("ecc.write_oob");
	return -EROFS;
}

static int t48s_nand_write_page(struct mtd_info *mtd, struct nand_chip *chip,
				const uint8_t *buf, int oob_required, int page,
				int cached, int raw)
{
	t48s_nand_refuse("write_page");
	return -EROFS;
}

static void t48s_nand_erase_cmd(struct mtd_info *mtd, int page)
{
	t48s_nand_refuse("erase_cmd");
}

static int t48s_nand_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	t48s_nand_refuse("block_markbad");
	return -EROFS;
}

/* layer 3 leftovers: the MTD core never calls these on a read-only device */
static int t48s_mtd_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	t48s_nand_refuse("mtd erase");
	return -EROFS;
}

/* kept as a stub, not NULL: without it the MTD core says the device has no spare
 * area at all and refuses raw reads */
static int t48s_mtd_write_oob(struct mtd_info *mtd, loff_t to, struct mtd_oob_ops *ops)
{
	t48s_nand_refuse("mtd write_oob");
	return -EROFS;
}

static int t48s_mtd_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	t48s_nand_refuse("mtd block_markbad");
	return -EROFS;
}

/* ---- reading ---- */

static int t48s_nand_dev_ready(struct mtd_info *mtd)
{
	return readl(umi + UMI_NAND_RCSR) & RCSR_RDY;
}

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

static int t48s_nand_read_page(struct mtd_info *mtd, struct nand_chip *chip,
			       uint8_t *buf, int oob_required, int page)
{
	unsigned int max_bitflips = 0;
	int step, i, stat;

	for (step = 0; step < chip->ecc.steps; step++) {
		uint8_t *data = buf + step * T48S_ECC_SIZE;
		uint8_t *oob = chip->oob_poi + step * T48S_OOB_PER_STEP;

		if (step)
			chip->cmdfunc(mtd, NAND_CMD_RNDOUT, step * T48S_ECC_SIZE, -1);
		/* reset the engine, then let it follow the data */
		writel(BCH_RD_ECC_VALID, umi + UMI_BCH_CTRL);
		writel(BCH_ECC_RD_EN, umi + UMI_BCH_CTRL);
		chip->read_buf(mtd, data, T48S_ECC_SIZE);
		writel(BCH_ECC_RD_EN | BCH_PAUSE_ECC_DEC, umi + UMI_BCH_CTRL);
		/* this step's spare bytes: 9 the engine must not see, then the ECC */
		chip->cmdfunc(mtd, NAND_CMD_RNDOUT,
			      mtd->writesize + step * T48S_OOB_PER_STEP, -1);
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
	return max_bitflips;
}

static struct nand_ecclayout t48s_nand_oob = {
	.eccbytes = 4 * T48S_ECC_BYTES,
	.eccpos = {  9, 10, 11, 12, 13, 14, 15, 25, 26, 27, 28, 29, 30, 31,
		    41, 42, 43, 44, 45, 46, 47, 57, 58, 59, 60, 61, 62, 63 },
	.oobfree = { { 1, 8 }, { 16, 9 }, { 32, 9 }, { 48, 9 } },
};

static uint8_t t48s_nand_ff[] = { 0xff };
static struct nand_bbt_descr t48s_nand_badblock = {
	.offs = 0,
	.len = 1,
	.pattern = t48s_nand_ff,
};

/* Yealink's layout (its mtdparts). mtd9 is a zero-size "boot" there; here it is the
 * whole chip, which keeps the other numbers the same under both kernels. */
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

static int __devinit t48s_nand_probe(struct platform_device *pdev)
{
	struct nand_chip *chip;
	struct mtd_info *mtd;
	loff_t ofs;
	int err, bad = 0;

	mtd = kzalloc(sizeof(*mtd) + sizeof(*chip), GFP_KERNEL);
	if (!mtd)
		return -ENOMEM;
	chip = (struct nand_chip *)&mtd[1];
	mtd->priv = chip;
	mtd->owner = THIS_MODULE;
	mtd->dev.parent = &pdev->dev;

	umi = ioremap(T48S_UMI_PHYS, 0x1000);
	if (!umi) {
		err = -ENOMEM;
		goto out_free;
	}
	pr_info("t48s_nand: read-only. controller as found: timing %08x, control %08x (write protect pin %s), status %08x, BCH n=%u k=%u t=%u\n",
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

	chip->IO_ADDR_R = umi + NAND_REG_DATA;
	chip->IO_ADDR_W = umi + NAND_REG_DATA;	/* never used: write_buf is a stub */
	chip->cmd_ctrl = t48s_nand_cmd_ctrl;
	chip->dev_ready = t48s_nand_dev_ready;
	chip->chip_delay = 0;
	chip->options = 0;
	chip->bbt_options = 0;			/* bad block table in RAM only */
	chip->write_buf = t48s_nand_write_buf;
	chip->block_markbad = t48s_nand_block_markbad;
	chip->write_page = t48s_nand_write_page;
	chip->ecc.mode = NAND_ECC_HW;
	chip->ecc.size = T48S_ECC_SIZE;
	chip->ecc.bytes = T48S_ECC_BYTES;
	chip->ecc.strength = T48S_ECC_STRENGTH;
	chip->ecc.layout = &t48s_nand_oob;
	chip->ecc.read_page = t48s_nand_read_page;
	chip->ecc.write_page = t48s_nand_ecc_write_page;
	chip->ecc.write_page_raw = t48s_nand_ecc_write_page;
	chip->ecc.write_oob = t48s_nand_ecc_write_oob;
	chip->ecc.write_oob_raw = t48s_nand_ecc_write_oob;
	chip->badblock_pattern = &t48s_nand_badblock;

	err = nand_scan_ident(mtd, 1, NULL);
	if (err) {
		pr_err("t48s_nand: no chip found (%d)\n", err);
		goto out_unmap;
	}
	if (mtd->writesize != 2048 || mtd->oobsize != 64 || mtd->erasesize != 0x20000 ||
	    mtd->size != 0x08000000) {
		pr_err("t48s_nand: not the chip this driver knows: page %u, spare %u, block %u, size %llu\n",
		       mtd->writesize, mtd->oobsize, mtd->erasesize,
		       (unsigned long long)mtd->size);
		err = -ENODEV;
		goto out_unmap;
	}
	chip->erase_cmd = t48s_nand_erase_cmd;	/* nand_scan_ident set the real one */

	err = nand_scan_tail(mtd);
	if (err) {
		pr_err("t48s_nand: scan failed (%d)\n", err);
		goto out_unmap;
	}

	/* layer 3: nothing above this driver may write, whatever the layers below do */
	mtd->flags &= ~MTD_WRITEABLE;
	mtd->_write = NULL;
	mtd->_write_oob = t48s_mtd_write_oob;
	mtd->_panic_write = NULL;
	mtd->_erase = t48s_mtd_erase;
	mtd->_block_markbad = t48s_mtd_block_markbad;
	if (chip->erase_cmd != t48s_nand_erase_cmd ||
	    chip->write_buf != t48s_nand_write_buf ||
	    chip->write_page != t48s_nand_write_page ||
	    chip->block_markbad != t48s_nand_block_markbad ||
	    chip->ecc.write_page != t48s_nand_ecc_write_page ||
	    chip->ecc.write_page_raw != t48s_nand_ecc_write_page ||
	    chip->ecc.write_oob != t48s_nand_ecc_write_oob ||
	    chip->ecc.write_oob_raw != t48s_nand_ecc_write_oob ||
	    (chip->bbt_options & NAND_BBT_USE_FLASH) ||
	    (mtd->flags & MTD_WRITEABLE)) {
		pr_err("t48s_nand: a write hook is not a stub; not registering the flash\n");
		nand_release(mtd);
		err = -EPERM;
		goto out_unmap;
	}

	mtd->name = "t48s-nand";
	err = mtd_device_register(mtd, t48s_nand_parts, ARRAY_SIZE(t48s_nand_parts));
	if (err) {
		nand_release(mtd);
		goto out_unmap;
	}
	t48s_mtd = mtd;
	for (ofs = 0; ofs < mtd->size; ofs += mtd->erasesize)
		if (mtd_block_isbad(mtd, ofs))
			bad++;
	pr_info("t48s_nand: %d bad blocks, %u chip commands blocked while scanning\n",
		bad, blocked);
	return 0;

out_unmap:
	iounmap(umi);
out_free:
	kfree(mtd);
	return err;
}

static struct platform_driver t48s_nand_driver = {
	.driver = {
		.name = "bcm-nand",	/* the device arch/arm/mach-bcmring/arch.c registers */
		.owner = THIS_MODULE,
	},
	.probe = t48s_nand_probe,
};
module_platform_driver(t48s_nand_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Yealink T48S NAND flash, read-only");
