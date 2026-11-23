// SPDX-License-Identifier: GPL-2.0
/*
 * Yealink T48S: U-Boot's BOOTMODE flag, and the one thing we may do to it.
 *
 * Why this exists (issue #7, docs/live-image.md). When the phone is started through
 * U-Boot's recovery mode (USB stick or TFTP), U-Boot first saves BOOTMODE=usbmode
 * or BOOTMODE=resumemode in its "userenv" flash partition. Yealink's firmware reads
 * that on its next start and, in resume mode always and in USB mode if a stick is
 * plugged in, erases the owner's settings and tries to reflash. Somebody trying our
 * firmware from a stick must get their phone back as it was, so the live image takes
 * the flag out again. Yealink's own start-up does exactly this on every boot
 * ("userenv -s BOOTMODE"); tools/rescue-init.sh does it with Yealink's tool.
 *
 * What this file can do, in full:
 *   /sys/kernel/t48s_bootmode/mode    read: the flag's value, "none", or "unreadable"
 *   /sys/kernel/t48s_bootmode/status  read: what the last clear did
 *   /sys/kernel/t48s_bootmode/clear   write the word BOOTMODE: take the flag out
 *
 * The clear takes nothing from userspace but that one word. The kernel reads the
 * 4 KB environment block itself, checks its CRC, builds the same list of variables
 * without BOOTMODE and hands that block to the flash driver. If the flag is not
 * set, nothing is written. It writes at most once per boot (a second try is allowed
 * only if the first left the block unreadable).
 *
 * The write itself is not here. It is t48s_nand_userenv_rewrite(), which the flash
 * driver provides if it can write (issue #29) and which must accept nothing but one
 * block inside userenv. The weak version below is what a read-only driver gets: it
 * refuses, "clear" fails with EROFS and the flash is not touched. So with this file
 * alone, every flash write is still impossible.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/crc32.h>
#include <linux/mtd/mtd.h>

#define t48s_env_crc(p, len)	(~crc32_le(~0U, (p), (len)))
#include "t48s_bootmode_env.h"

#define T48S_USERENV_OFS	0x00220000	/* where userenv starts on the chip */
#define T48S_USERENV_SIZE	0x00100000

/*
 * For the flash driver to provide. Erase the one 128 KB block at chip offset
 * block_ofs, which must lie inside userenv (the driver checks this itself and
 * refuses anything else), and program len bytes (whole pages) at its start with
 * the driver's usual error correction, erased check and read-back. 0 or -errno.
 */
int __weak t48s_nand_userenv_rewrite(loff_t block_ofs, const u8 *data, size_t len)
{
	return -EROFS;
}

static DEFINE_MUTEX(t48s_bootmode_lock);
static int t48s_bootmode_writes;	/* rewrites asked of the flash driver this boot */
static char t48s_bootmode_status[96] = "not tried";

/* Read the environment block as U-Boot does: from the first good block. */
static int t48s_bootmode_read(u8 *env, loff_t *block)
{
	struct mtd_info *mtd = get_mtd_device_nm("userenv");
	size_t got = 0;
	loff_t ofs;
	int err = -ENODEV;

	if (IS_ERR(mtd))
		return PTR_ERR(mtd);
	for (ofs = 0; ofs < mtd->size; ofs += mtd->erasesize) {
		if (mtd_block_isbad(mtd, ofs))
			continue;
		err = mtd_read(mtd, ofs, T48S_ENV_SIZE, &got, env);
		if (mtd_is_bitflip(err))
			err = 0;
		if (!err && got != T48S_ENV_SIZE)
			err = -EIO;
		*block = ofs;
		break;
	}
	put_mtd_device(mtd);
	return err;
}

static ssize_t mode_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u8 *env = kmalloc(T48S_ENV_SIZE, GFP_KERNEL);
	char val[32];
	loff_t block;
	ssize_t n;

	if (!env)
		return -ENOMEM;
	mutex_lock(&t48s_bootmode_lock);
	if (t48s_bootmode_read(env, &block) || !t48s_env_valid(env))
		n = sprintf(buf, "unreadable\n");
	else if (t48s_env_get(env, "BOOTMODE", val, sizeof(val)))
		n = sprintf(buf, "%s\n", val);
	else
		n = sprintf(buf, "none\n");
	mutex_unlock(&t48s_bootmode_lock);
	kfree(env);
	return n;
}

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", t48s_bootmode_status);
}

static void t48s_bootmode_say(const char *what)
{
	strlcpy(t48s_bootmode_status, what, sizeof(t48s_bootmode_status));
	pr_notice("t48s_bootmode: %s\n", what);
}

static ssize_t clear_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	u8 *env, *new, *check;
	const unsigned char *p;
	char val[32];
	loff_t block;
	int err;

	if (!sysfs_streq(buf, "BOOTMODE"))
		return -EINVAL;
	env = kmalloc(3 * T48S_ENV_SIZE, GFP_KERNEL);
	if (!env)
		return -ENOMEM;
	new = env + T48S_ENV_SIZE;
	check = new + T48S_ENV_SIZE;

	mutex_lock(&t48s_bootmode_lock);
	err = t48s_bootmode_read(env, &block);
	if (err || !t48s_env_valid(env)) {
		/* never rewrite what we cannot read: there is nothing to keep */
		t48s_bootmode_say("userenv is unreadable; left alone");
		err = err ? err : -EIO;
		goto out;
	}
	if (!t48s_env_get(env, "BOOTMODE", val, sizeof(val))) {
		t48s_bootmode_say("BOOTMODE is not set; nothing written");
		goto out;
	}
	if (t48s_bootmode_writes) {
		t48s_bootmode_say("already written once this boot; left alone");
		err = -EBUSY;
		goto out;
	}
	t48s_env_remove(env, new, "BOOTMODE");
	if (!t48s_env_valid(new) || t48s_env_get(new, "BOOTMODE", val, sizeof(val))) {
		t48s_bootmode_say("internal error building the new block; nothing written");
		err = -EINVAL;
		goto out;
	}
	/* on the serial log, so the list can be put back by hand if the worst happens */
	pr_notice("t48s_bootmode: rewriting userenv without BOOTMODE; it keeps:\n");
	for (p = new + 4; *p; p = t48s_env_next(new, p))
		pr_notice("t48s_bootmode:   %s\n", (const char *)p);

	t48s_bootmode_writes++;
	err = t48s_nand_userenv_rewrite(T48S_USERENV_OFS + block, new, T48S_ENV_SIZE);
	if (err == -EROFS) {
		t48s_bootmode_say("this kernel's flash driver cannot write; BOOTMODE is still set");
		goto out;
	}
	if (err || t48s_bootmode_read(check, &block) || memcmp(check, new, T48S_ENV_SIZE)) {
		/* the block may be erased now: one more go at the same contents */
		pr_err("t48s_bootmode: first rewrite failed (%d); trying once more\n", err);
		err = t48s_nand_userenv_rewrite(T48S_USERENV_OFS + block, new, T48S_ENV_SIZE);
		if (err || t48s_bootmode_read(check, &block) ||
		    memcmp(check, new, T48S_ENV_SIZE)) {
			t48s_bootmode_say("REWRITE FAILED: userenv may be empty; the list is in the kernel log");
			err = err ? err : -EIO;
			goto out;
		}
	}
	t48s_bootmode_say("BOOTMODE cleared");
out:
	mutex_unlock(&t48s_bootmode_lock);
	kfree(env);
	return err ? err : count;
}

static struct kobj_attribute mode_attr = __ATTR(mode, 0444, mode_show, NULL);
static struct kobj_attribute status_attr = __ATTR(status, 0444, status_show, NULL);
static struct kobj_attribute clear_attr = __ATTR(clear, 0200, NULL, clear_store);
static struct attribute *t48s_bootmode_attrs[] = {
	&mode_attr.attr, &status_attr.attr, &clear_attr.attr, NULL
};
static struct attribute_group t48s_bootmode_group = { .attrs = t48s_bootmode_attrs };

static int __init t48s_bootmode_init(void)
{
	struct kobject *kobj = kobject_create_and_add("t48s_bootmode", kernel_kobj);

	BUILD_BUG_ON(T48S_ENV_SIZE > 0x20000);
	if (!kobj)
		return -ENOMEM;
	return sysfs_create_group(kobj, &t48s_bootmode_group);
}
late_initcall(t48s_bootmode_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Yealink T48S: read U-Boot's BOOTMODE flag and clear it");
