// SPDX-License-Identifier: GPL-2.0
/*
 * t48s_flip: page flipping on the Yealink SIP-T48S screen, /dev/t48flip.
 *
 * The boot loader leaves the SoC's ARM PL111 running (800x480, one 32-bit word
 * per pixel) and reserves room for four frames above the RAM Linux uses. The
 * first frame is /dev/fb0 (simple-framebuffer). This driver offers the other
 * three as buffers a program can draw into and have shown whole, with no
 * tearing and no copy:
 *
 *   mmap      the buffers, one after another from offset 0. The mapping is
 *             cached (five times faster than fb0's for anything that reads
 *             back what it drew, measured on the phone); T48FLIP_SHOW writes
 *             the cache out before the controller is pointed at the buffer.
 *   T48FLIP_INFO   size and number of the buffers, and the refresh period.
 *   T48FLIP_SHOW   show buffer n (0, 1, ...), or -1 for fb0's frame. The
 *             controller takes a new address (LCDUPBASE) at the start of the
 *             next frame and raises "next base update" (LNBU) when it has,
 *             so nothing changes part way down the screen.
 *   T48FLIP_CLEAR  all buffers to black.
 *   read, poll     one struct t48flip_event for each T48FLIP_SHOW that has
 *             reached the screen: which buffer, the frame count and the time.
 *             A SHOW made before the last one arrived replaces it.
 *
 * SHOW, CLEAR and read need CAP_SYS_ADMIN: the shell opens the device and
 * hands the descriptor to an app, which can then map the buffers and nothing
 * else. When the last descriptor is closed the screen goes back to fb0.
 *
 * The controller's timing is never written. Its size and depth are read from
 * its registers when T48FLIP_INFO is first asked, and a controller that is
 * off, or in another depth, gives ENODEV.
 *
 * Measured on the phone: 67.8 frames a second; LNBU comes at the first line
 * of the new frame, with LCDUPCURR already inside the new buffer.
 *
 * Device tree: "yealink,t48s-flip", reg (the PL111), interrupts, and
 * memory-region (the buffers: reserved, no-map).
 *
 * GPL v2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <asm/cacheflush.h>

#define DRV_NAME	"t48s_flip"

/* what a program sees; the shell has the same in shell/be_fb.c */
struct t48flip_info {
	__u32 width, height;	/* pixels */
	__u32 stride;		/* bytes from one row to the next */
	__u32 count;		/* buffers */
	__u32 size;		/* bytes from one buffer to the next */
	__u32 period_ns;	/* one refresh of the screen */
};
struct t48flip_event {
	__s32 buffer;		/* now on the screen; -1: fb0's frame */
	__u32 frame;		/* refreshes counted while the interrupt is on */
	__u64 time_ns;		/* CLOCK_MONOTONIC at that refresh */
};
#define T48FLIP_INFO	_IOR('T', 0x48, struct t48flip_info)
#define T48FLIP_SHOW	_IOW('T', 0x49, __s32)
#define T48FLIP_CLEAR	_IO('T', 0x4a)

#define CLCD_TIM0	0x00
#define CLCD_TIM1	0x04
#define CLCD_UPBASE	0x10
#define CLCD_CNTL	0x18	/* PL111 order: the PL110 has these two the other way round */
#define CLCD_IMSC	0x1c
#define CLCD_MIS	0x24
#define CLCD_ICR	0x28
#define CLCD_UPCURR	0x2c
#define CNTL_EN		0x001
#define CNTL_BPP	0x00e
#define CNTL_BPP24	0x00a	/* 5: 24 bits in a 32-bit word */
#define CNTL_TFT	0x020
#define INT_LNBU	0x004

#define MAX_BUFFERS	8

static void __iomem *regs;
static phys_addr_t mem_phys;
static size_t mem_len;
static void *mem_wc;			/* our own mapping, for clearing */

static DEFINE_MUTEX(setup_lock);
static bool ready;
static struct t48flip_info info;
static u32 home;			/* fb0's frame: where the controller pointed when we first looked */

static DEFINE_SPINLOCK(lock);		/* everything below, and the interrupt mask */
static bool irq_on, pending, have_event, counting;
static int want = -1, shown = -1;
static u32 want_base;
static u32 frame;
static u64 stamp;			/* ns, at the last interrupt */
static struct t48flip_event event;
static DECLARE_WAIT_QUEUE_HEAD(waitq);
static int users;

static irqreturn_t flip_irq(int irq, void *dev)
{
	u32 cur;

	if (!(readl(regs + CLCD_MIS) & INT_LNBU))
		return IRQ_NONE;
	spin_lock(&lock);
	writel(INT_LNBU, regs + CLCD_ICR);
	frame++;
	stamp = ktime_get_ns();
	/* The frame that has just begun: is it the one asked for? If we are
	 * late and the frame is over, LCDUPCURR reads 0 and we look again
	 * next time. */
	cur = readl(regs + CLCD_UPCURR);
	if (pending && cur >= want_base && cur - want_base < info.size) {
		pending = false;
		shown = want;
		event.buffer = shown;
		event.frame = frame;
		event.time_ns = stamp;
		have_event = true;
	}
	if (!pending && shown < 0 && !counting) {
		/* back on fb0 with nothing asked: no need to count frames */
		writel(0, regs + CLCD_IMSC);
		irq_on = false;
	}
	spin_unlock(&lock);
	wake_up_interruptible(&waitq);
	return IRQ_HANDLED;
}

/* Write every dirty line of the data cache to RAM and leave the cache as it
 * is: one ARM1176 operation, far cheaper than walking a 1.5 MB buffer line by
 * line, and it does not touch the instruction cache as flush_cache_all() does. */
static void dcache_clean_all(void)
{
	asm volatile("mcr p15, 0, %0, c7, c10, 0\n\tmcr p15, 0, %0, c7, c10, 4" : : "r" (0));
}

/* with the lock held */
static void irq_start(void)
{
	if (irq_on)
		return;
	writel(INT_LNBU, regs + CLCD_ICR);
	writel(INT_LNBU, regs + CLCD_IMSC);
	irq_on = true;
}

static void show(int n)
{
	unsigned long flags;

	/* what was drawn through a cached mapping must be in RAM before the
	 * controller reads it */
	dcache_clean_all();
	spin_lock_irqsave(&lock, flags);
	want = n;
	want_base = n < 0 ? home : (u32)mem_phys + (u32)n * info.size;
	pending = true;
	have_event = false;
	writel(want_base, regs + CLCD_UPBASE);
	irq_start();
	spin_unlock_irqrestore(&lock, flags);
}

/* Read what the boot loader set up, once, and time the refresh. */
static int flip_setup(void)
{
	u32 tim0, tim1, cntl, f0, f1;
	u64 t0, t1;
	unsigned long flags;
	int rc = 0;

	mutex_lock(&setup_lock);
	if (ready)
		goto out;
	rc = -ENODEV;
	tim0 = readl(regs + CLCD_TIM0);
	tim1 = readl(regs + CLCD_TIM1);
	cntl = readl(regs + CLCD_CNTL);
	home = readl(regs + CLCD_UPBASE);
	if ((cntl & (CNTL_EN | CNTL_TFT | CNTL_BPP)) != (CNTL_EN | CNTL_TFT | CNTL_BPP24) || !home) {
		pr_info(DRV_NAME ": the LCD controller is not showing 32-bit pixels (control 0x%x)\n", cntl);
		goto out;
	}
	info.width = (((tim0 >> 2) & 0x3f) + 1) * 16;
	info.height = (tim1 & 0x3ff) + 1;
	info.stride = info.width * 4;
	info.size = PAGE_ALIGN(info.stride * info.height);
	info.count = min_t(u32, mem_len / info.size, MAX_BUFFERS);
	if (info.count < 2 || (home >= mem_phys && home - mem_phys < mem_len)) {
		pr_info(DRV_NAME ": %ux%u, %zu bytes at 0x%08x, frame at 0x%08x: no room to flip\n",
			info.width, info.height, mem_len, (u32)mem_phys, home);
		goto out;
	}

	/* the refresh period, over a third of a second */
	spin_lock_irqsave(&lock, flags);
	counting = true;		/* keeps the interrupt on */
	irq_start();
	spin_unlock_irqrestore(&lock, flags);
	msleep(50);
	spin_lock_irqsave(&lock, flags);
	f0 = frame;
	t0 = stamp;
	spin_unlock_irqrestore(&lock, flags);
	msleep(330);
	spin_lock_irqsave(&lock, flags);
	f1 = frame;
	t1 = stamp;
	counting = false;
	spin_unlock_irqrestore(&lock, flags);
	if (f1 - f0 < 3) {
		pr_info(DRV_NAME ": no frame interrupts from the LCD controller\n");
		spin_lock_irqsave(&lock, flags);
		writel(0, regs + CLCD_IMSC);
		irq_on = false;
		spin_unlock_irqrestore(&lock, flags);
		goto out;
	}
	info.period_ns = (u32)div_u64(t1 - t0, f1 - f0);
	pr_info(DRV_NAME ": %ux%u, %u buffers of %u bytes at 0x%08x, refresh %u.%03u ms\n",
		info.width, info.height, info.count, info.size, (u32)mem_phys,
		info.period_ns / 1000000, info.period_ns / 1000 % 1000);
	ready = true;
	rc = 0;
out:
	mutex_unlock(&setup_lock);
	return rc;
}

static int flip_open(struct inode *inode, struct file *file)
{
	unsigned long flags;

	spin_lock_irqsave(&lock, flags);
	users++;
	spin_unlock_irqrestore(&lock, flags);
	return 0;
}

static int flip_release(struct inode *inode, struct file *file)
{
	unsigned long flags;
	bool last;

	spin_lock_irqsave(&lock, flags);
	last = --users == 0;
	spin_unlock_irqrestore(&lock, flags);
	/* nobody left to ask for fb0 back */
	if (last && ready)
		show(-1);
	return 0;
}

static long flip_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	s32 n;
	int rc;

	rc = flip_setup();
	if (rc)
		return rc;
	switch (cmd) {
	case T48FLIP_INFO:
		return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
	case T48FLIP_SHOW:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(n, (s32 __user *)arg))
			return -EFAULT;
		if (n < -1 || n >= (s32)info.count)
			return -EINVAL;
		show(n);
		return 0;
	case T48FLIP_CLEAR:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		/* nothing older may be written over this later, or read back from the cache */
		flush_cache_all();
		memset(mem_wc, 0, (size_t)info.count * info.size);
		return 0;
	}
	return -ENOTTY;
}

static ssize_t flip_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	struct t48flip_event ev;
	unsigned long flags;
	bool got;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (len < sizeof(ev))
		return -EINVAL;
	for (;;) {
		spin_lock_irqsave(&lock, flags);
		got = have_event;
		ev = event;
		have_event = false;
		spin_unlock_irqrestore(&lock, flags);
		if (got)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(waitq, have_event))
			return -ERESTARTSYS;
	}
	return copy_to_user(buf, &ev, sizeof(ev)) ? -EFAULT : sizeof(ev);
}

static __poll_t flip_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &waitq, wait);
	return have_event ? EPOLLIN | EPOLLRDNORM : 0;
}

static int flip_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long off = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long total;
	int rc;

	rc = flip_setup();
	if (rc)
		return rc;
	total = (unsigned long)info.count * info.size;
	if (vma->vm_pgoff > total >> PAGE_SHIFT || len > total - off)
		return -EINVAL;
	/* cached, as ordinary memory: see T48FLIP_SHOW */
	return remap_pfn_range(vma, vma->vm_start, (mem_phys + off) >> PAGE_SHIFT, len, vma->vm_page_prot);
}

static const struct file_operations flip_fops = {
	.owner		= THIS_MODULE,
	.open		= flip_open,
	.release	= flip_release,
	.unlocked_ioctl	= flip_ioctl,
	.read		= flip_read,
	.poll		= flip_poll,
	.mmap		= flip_mmap,
	.llseek		= noop_llseek,
};

static struct miscdevice flip_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "t48flip",
	.fops	= &flip_fops,
	.mode	= 0600,
};

static int t48s_flip_probe(struct platform_device *pdev)
{
	struct device_node *mem;
	struct resource res;
	int irq, rc;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	mem = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!mem)
		return -ENODEV;
	rc = of_address_to_resource(mem, 0, &res);
	of_node_put(mem);
	if (rc)
		return rc;
	mem_phys = res.start;
	mem_len = resource_size(&res);
	if (!PAGE_ALIGNED(mem_phys))
		return -EINVAL;
	mem_wc = devm_memremap(&pdev->dev, mem_phys, mem_len, MEMREMAP_WC);
	if (IS_ERR(mem_wc))
		return PTR_ERR(mem_wc);

	writel(0, regs + CLCD_IMSC);
	rc = devm_request_irq(&pdev->dev, irq, flip_irq, 0, DRV_NAME, &flip_dev);
	if (rc)
		return rc;
	return misc_register(&flip_dev);
}

static const struct of_device_id t48s_flip_of_match[] = {
	{ .compatible = "yealink,t48s-flip" },
	{ }
};

static struct platform_driver t48s_flip_driver = {
	.probe = t48s_flip_probe,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = t48s_flip_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(t48s_flip_driver);
