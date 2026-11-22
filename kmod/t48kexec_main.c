/*
 * t48kexec: boot another kernel from Yealink's running 2.6.27 kernel (no kexec
 * support built in there), with the SoC watchdog armed as a safety net.
 *
 *   insmod t48kexec.ko image=/tmp/Image wdt=60
 *
 * image: an uncompressed ARM kernel Image (with its initramfs built in), loaded
 *        to physical 0x00408000 and entered with r0=0, r1=2342 (machine), r2=0.
 * wdt:   seconds until the SP805 watchdog resets the board (0 = leave it off).
 *        The reset happens after two timeouts, so each timeout is wdt/2.
 *
 * Steps: the image is read into single pages that lie outside the destination;
 * a stub (t48kexec_stub.S) and the list of (dest, src) pairs go into one more
 * block, mapped 1:1 in the current page table. With interrupts off and caches
 * flushed we jump to the stub's physical address. The stub turns the MMU and
 * caches off, copies the pages, arms the watchdog and enters the new kernel.
 * There is no way back except the watchdog or the new kernel's own reboot.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/gfp.h>
#include <linux/sched.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#define DEST_PHYS   0x00408000UL
#define MACH_NR     2342
#define WDT_CLK_HZ  25000000UL   /* measured: 0x0EE6B280 counts per 10 s */

/* Yealink's kernel has RAM from physical 0x00400000 at PAGE_OFFSET. The build tree
 * (an OMAP config, RAM at 0x80000000) would compile virt_to_phys()/page_to_phys()
 * with the wrong offset, so convert by hand. page_address() is fine: its offsets
 * cancel out. */
#define Y_PHYS_OFFSET 0x00400000UL
#define y_virt_to_phys(v) ((unsigned long)(v) - PAGE_OFFSET + Y_PHYS_OFFSET)
#define y_page_to_phys(pg) y_virt_to_phys(page_address(pg))

static char *image = "/tmp/Image";
static int wdt = 60;
static int dry;
module_param(image, charp, 0);
module_param(wdt, int, 0);
module_param(dry, int, 0);	/* 1: do everything except arm the watchdog and jump */

extern char t48_stub_start[], t48_stub_end[];

/* Layout of the stub block: code, then a parameter area, then the page list */
struct stub_params {
	unsigned long entry;      /* where to jump: DEST_PHYS */
	unsigned long mach;       /* r1 */
	unsigned long wdt_load;   /* SP805 load value, 0 = do not arm */
	unsigned long npages;     /* number of (dest, src) pairs that follow */
	unsigned long pairs[0];   /* dest0, src0, dest1, src1, ... (physical) */
};

#define STUB_ORDER 3              /* 32 KB: code + params + up to ~3.9k pairs */
#define STUB_CODE_MAX 1024

static int in_dest(unsigned long phys, unsigned long size)
{
	return phys + PAGE_SIZE > DEST_PHYS && phys < DEST_PHYS + size + PAGE_SIZE;
}

static int __init t48kexec_init(void)
{
	struct file *f;
	loff_t size;
	unsigned long npages, i, stub_virt = 0, stub_phys;
	struct stub_params *p;
	struct page **pages = NULL, **rejects = NULL;
	unsigned long nrejects = 0;
	int ret = -ENOMEM;
	void (*jump)(void);
	pgd_t *pgd;
	unsigned long sect;

	if (t48_stub_end - t48_stub_start > STUB_CODE_MAX) {
		printk(KERN_ERR "t48kexec: stub too big\n");
		return -EINVAL;
	}

	f = filp_open(image, O_RDONLY, 0);
	if (IS_ERR(f)) {
		printk(KERN_ERR "t48kexec: cannot open %s\n", image);
		return PTR_ERR(f);
	}
	size = f->f_path.dentry->d_inode->i_size;
	npages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	printk(KERN_INFO "t48kexec: %s is %lld bytes (%lu pages)\n", image, size, npages);
	if (npages == 0 || npages * 2 * sizeof(long) + sizeof(*p) + STUB_CODE_MAX > (PAGE_SIZE << STUB_ORDER)) {
		printk(KERN_ERR "t48kexec: image size not supported\n");
		ret = -EINVAL;
		goto out_file;
	}

	pages = vmalloc(npages * sizeof(*pages));
	rejects = vmalloc((npages + 64) * sizeof(*rejects));
	if (!pages || !rejects)
		goto out_free;
	memset(pages, 0, npages * sizeof(*pages));

	/* the stub block must not lie in the destination either */
	for (;;) {
		stub_virt = __get_free_pages(GFP_KERNEL, STUB_ORDER);
		if (!stub_virt)
			goto out_free;
		if (!in_dest(y_virt_to_phys((void *)stub_virt), size) &&
		    !in_dest(y_virt_to_phys((void *)stub_virt) + (PAGE_SIZE << STUB_ORDER) - PAGE_SIZE, size))
			break;
		/* leak it on purpose: we never come back */
	}
	stub_phys = y_virt_to_phys((void *)stub_virt);

	/* read the image, page by page, into pages outside the destination */
	for (i = 0; i < npages; i++) {
		struct page *pg;
		for (;;) {
			pg = alloc_page(GFP_KERNEL);
			if (!pg)
				goto out_free;
			if (!in_dest(y_page_to_phys(pg), size))
				break;
			if (nrejects < npages + 64)
				rejects[nrejects++] = pg;
		}
		pages[i] = pg;
		memset(page_address(pg), 0, PAGE_SIZE);
		ret = kernel_read(f, i * PAGE_SIZE, page_address(pg),
				  min_t(loff_t, PAGE_SIZE, size - i * PAGE_SIZE));
		if (ret < 0) {
			printk(KERN_ERR "t48kexec: read failed at page %lu\n", i);
			goto out_free;
		}
	}
	filp_close(f, NULL);
	f = NULL;

	/* stub code, then parameters and page list right after it */
	memcpy((void *)stub_virt, t48_stub_start, t48_stub_end - t48_stub_start);
	p = (struct stub_params *)(stub_virt + STUB_CODE_MAX);
	p->entry = DEST_PHYS;
	p->mach = MACH_NR;
	p->wdt_load = wdt > 0 ? (WDT_CLK_HZ * (unsigned long)wdt) / 2 : 0;
	p->npages = npages;
	for (i = 0; i < npages; i++) {
		p->pairs[2 * i] = DEST_PHYS + i * PAGE_SIZE;
		p->pairs[2 * i + 1] = y_page_to_phys(pages[i]);
	}
	printk(KERN_INFO "t48kexec: stub at phys 0x%08lx, %lu pages to 0x%08lx, watchdog %d s; jumping\n",
	       stub_phys, npages, DEST_PHYS, wdt);

	if (dry) {
		printk(KERN_INFO "t48kexec: dry run: first pair dest 0x%08lx src 0x%08lx, last dest 0x%08lx; "
		       "stub code %d bytes, %lu rejected pages; not jumping\n",
		       p->pairs[0], p->pairs[1], p->pairs[2 * (npages - 1)],
		       (int)(t48_stub_end - t48_stub_start), nrejects);
		ret = -EAGAIN;	/* refuse to stay loaded; frees everything below */
		goto out_free;
	}

	/* arm the watchdog now, so a failure anywhere from here on still ends in a
	 * reset (the stub reloads it after copying) */
	if (p->wdt_load) {
		void __iomem *wd = ioremap(0x80017000, 0x1000);
		if (wd) {
			writel(0x1ACCE551, wd + 0xc00);	/* unlock */
			writel(p->wdt_load, wd + 0x800);	/* load */
			writel(3, wd + 0x808);			/* interrupt + reset enable */
			writel(0, wd + 0xc00);			/* lock */
			printk(KERN_INFO "t48kexec: watchdog armed\n");
		}
	}

	/* Yealink's Linux leaves UARTA at 9600 baud; set 115200 (100 MHz UART clock:
	 * divisor 54 + 16/64) so the new kernel's early console is readable. Writing
	 * LCR_H latches the divisor. */
	{
		void __iomem *u = ioremap(0x80012000, 0x1000);
		if (u) {
			while (readl(u + 0x18) & 0x08)	/* FR.BUSY: let the last printk drain */
				;
			writel(54, u + 0x24);		/* IBRD */
			writel(16, u + 0x28);		/* FBRD */
			writel(0x70, u + 0x2c);		/* LCR_H: 8N1, FIFOs on */
		}
	}

	/* map the stub's megabyte 1:1 in the current page table (a user address, so
	 * nothing the kernel needs on the way out is disturbed) */
	pgd = current->active_mm->pgd;
	sect = stub_phys & 0xfff00000;
	((unsigned long *)pgd)[sect >> 20] = sect | PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ;

	local_irq_disable();
	local_fiq_disable();

	/* clean and invalidate the whole data cache, invalidate the I cache and TLB */
	__asm__ __volatile__(
		"mov	r0, #0\n"
		"mcr	p15, 0, r0, c7, c14, 0\n"	/* clean+invalidate D */
		"mcr	p15, 0, r0, c7, c5, 0\n"	/* invalidate I */
		"mcr	p15, 0, r0, c7, c10, 4\n"	/* drain write buffer */
		"mcr	p15, 0, r0, c8, c7, 0\n"	/* invalidate TLBs */
		"mcr	p15, 0, r0, c7, c5, 4\n"	/* flush prefetch buffer */
		: : : "r0", "memory");

	jump = (void (*)(void))stub_phys;
	jump();
	/* not reached */
	return 0;

out_free:
	for (i = 0; pages && i < npages; i++)
		if (pages[i])
			__free_page(pages[i]);
	for (i = 0; i < nrejects; i++)
		__free_page(rejects[i]);
	if (stub_virt)
		free_pages(stub_virt, STUB_ORDER);
	if (pages)
		vfree(pages);
	if (rejects)
		vfree(rejects);
out_file:
	if (f)
		filp_close(f, NULL);
	return ret < 0 ? ret : -ENOMEM;
}

module_init(t48kexec_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Boot another kernel from the running one (Yealink T48S)");
