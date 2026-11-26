/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Yealink T4xS flash driver: is this phone's flash laid out as the driver's
 * table says, and is a kernel image meant for this model? (issue #10,
 * docs/models.md). Plain functions with no kernel dependencies, so that
 * tools/test-layout.sh can run them on a PC.
 *
 * The driver's partition table and its writeable ranges were read from one
 * T48S. Other phones (a T46S, a T48S with another U-Boot) are not written on
 * trust: U-Boot's own environment, in the first block of "env", names the
 * layout it and Yealink's kernel use (mtdparts) and the place U-Boot starts
 * the kernel from (bootnand). Only when the environment's CRC is right and
 * all three of these are exactly what the driver expects does the driver
 * open anything for writing. Otherwise everything stays read-only, the
 * userenv window included, and the installer refuses.
 *
 * The second rule is about our own kernels: a kernel image named for one
 * model (kernel6/build.sh names it Linux-VERSION-t46s or ...-t48s) is never
 * written as page 0 of the kernel partition of the other.
 */
#ifndef T48S_NAND_LAYOUT_H
#define T48S_NAND_LAYOUT_H

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#define T48S_ENV_OFS		0x00120000LL	/* env on the chip, its first block */
#define T48S_ENV_LEN		0x1000		/* the environment block: CRC and variables */
#define T48S_KERNEL_OFS		0x015a0000LL	/* the kernel partition, where nboot reads */

/* U-Boot's mtdparts as the driver's table has it (t48s_nand_parts[]; "boot" is
 * Yealink's empty mtd9). tools/test-layout.sh checks it against tools/t48flash.py. */
static const char t48s_layout_mtdparts[] =
	"mtdparts=bcm_umi_nand.0:"
	"0x00020000@0x00000000(ubl)rw,0x00100000@0x00020000(uboot)rw,"
	"0x00100000@0x00120000(env)rw,0x00100000@0x00220000(userenv)rw,"
	"0x00100000@0x00320000(logo)rw,0x00280000@0x00420000(smkern)rw,"
	"0x00F00000@0x006A0000(smrfs)rw,0x00280000@0x015A0000(kernel)rw,"
	"0x00F00000@0x01820000(rfs)rw,0x00000000@0x02720000(boot)rw,"
	"0x04AE0000@0x02720000(app)rw,0x00400000@0x07200000(cfg)rw,"
	"0x00A00000@0x07600000(data)rw;";
static const char t48s_layout_bootcmd[] = "run bootnand";
/* how bootnand must end: the kernel partition, started as it is read */
static const char t48s_layout_nboot[] = "; setenv autostart yes; nboot 0x02680000 0 0x015A0000";

/* U-Boot's CRC-32 (zlib's) */
static unsigned int t48s_layout_crc32(const unsigned char *p, unsigned long n)
{
	unsigned int c = 0xffffffffu;
	int k;

	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
	}
	return ~c;
}

/*
 * env: the first T48S_ENV_LEN bytes of the env partition. NULL if they are a
 * U-Boot environment naming exactly this layout, else why not, in a few words.
 */
static const char *t48s_layout_check(const unsigned char *env, unsigned long len)
{
	unsigned long i, n, mtdparts = 0, bootcmd = 0, bootnand = 0;
	unsigned long nb = sizeof(t48s_layout_nboot) - 1;
	const char *v;

	if (!env || len != T48S_ENV_LEN)
		return "not one environment block";
	if ((env[0] | env[1] << 8 | env[2] << 16 | (unsigned int)env[3] << 24) !=
	    t48s_layout_crc32(env + 4, len - 4))
		return "no environment (CRC wrong)";
	for (i = 4; i < len && env[i]; i += n + 1) {
		v = (const char *)env + i;
		for (n = 0; i + n < len && env[i + n]; n++)
			;
		if (i + n >= len)
			return "environment not terminated";
		if (n >= 9 && !memcmp(v, "mtdparts=", 9)) {
			if (mtdparts++ || n - 9 != sizeof(t48s_layout_mtdparts) - 1 ||
			    memcmp(v + 9, t48s_layout_mtdparts, n - 9))
				return "mtdparts is not this layout";
		} else if (n >= 8 && !memcmp(v, "bootcmd=", 8)) {
			if (bootcmd++ || n - 8 != sizeof(t48s_layout_bootcmd) - 1 ||
			    memcmp(v + 8, t48s_layout_bootcmd, n - 8))
				return "bootcmd is not run bootnand";
		} else if (n >= 9 && !memcmp(v, "bootnand=", 9)) {
			if (bootnand++ || n - 9 < nb || memcmp(v + n - nb, t48s_layout_nboot, nb))
				return "bootnand does not start the kernel partition";
		}
	}
	if (i >= len)
		return "environment not terminated";
	if (!mtdparts)
		return "no mtdparts";
	if (!bootcmd)
		return "no bootcmd";
	if (!bootnand)
		return "no bootnand";
	return NULL;
}

/* the value of the first variable called name ("mtdparts") in a block that
 * passed its CRC, and its length; NULL if there is none. For the log */
static const char *t48s_layout_var(const unsigned char *env, unsigned long len,
				   const char *name, unsigned long *vlen)
{
	unsigned long i, n, k = strlen(name);

	for (i = 4; i < len && env[i]; i += n + 1) {
		for (n = 0; i + n < len && env[i + n]; n++)
			;
		if (i + n >= len)
			return NULL;
		if (n > k && env[i + k] == '=' && !memcmp(env + i, name, k)) {
			*vlen = n - k - 1;
			return (const char *)env + i + k + 1;
		}
	}
	return NULL;
}

/*
 * page: the first bytes (64 at least) of what is about to become page 0 of the
 * kernel partition. model: "t46s" or "t48s", what this kernel's device tree
 * says the phone is, or NULL. 0 if it is one of our kernels named for another
 * model (or for any model, when this one is not known); 1 otherwise. Anything
 * that is not a U-Boot image, or not named for a model (Yealink's own kernel,
 * from a backup), is not this rule's business.
 */
static int t48s_layout_kernel_ok(const unsigned char *page, const char *model)
{
	const char *name = (const char *)page + 32;
	unsigned long n;

	if (page[0] != 0x27 || page[1] != 0x05 || page[2] != 0x19 || page[3] != 0x56)
		return 1;
	for (n = 0; n < 32 && name[n]; n++)
		;
	if (n < 5 || name[n - 5] != '-' || name[n - 4] != 't' || name[n - 3] != '4' ||
	    name[n - 2] < '0' || name[n - 2] > '9' || name[n - 1] != 's')
		return 1;
	return model && strlen(model) == 4 && !memcmp(name + n - 4, model, 4);
}

#endif
