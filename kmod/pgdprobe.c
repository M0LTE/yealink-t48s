/* pgdprobe: print the first-level page table entries (Yealink kernel, swapper_pg_dir at
 * PAGE_OFFSET + 0x4000) for the I/O virtual addresses Yealink's drivers use, to learn the
 * physical addresses behind them. Read only. */
#include <linux/module.h>
#include <linux/kernel.h>
static unsigned long vas[] = {0xf3300000, 0xf3400000, 0xf3410000, 0xf3480000, 0xf34a0000, 0xf8000000, 0xf8008000, 0xf8010000};
static int __init pgdprobe_init(void)
{
	unsigned long *pgd = (unsigned long *)(PAGE_OFFSET + 0x4000);
	unsigned long va;
	/* every 1 MB of the I/O region, plus the vector/fixmap top */
	for (va = 0xe0000000; va >= 0xe0000000 && va < 0xfff00000; va += 0x100000) {
		unsigned long d = pgd[va >> 20];
		if ((d & 3) == 2)
			printk(KERN_INFO "pgdmap: va 0x%08lx section phys 0x%08lx desc 0x%08lx\n", va, d & 0xfff00000, d);
		else if ((d & 3) == 1) {
			unsigned long *pte = (unsigned long *)(PAGE_OFFSET + (d & 0xfffffc00) - 0x00400000);
			int i;
			for (i = 0; i < 256; i++)
				if (pte[i] & 2)
					printk(KERN_INFO "pgdmap: va 0x%08lx page phys 0x%08lx\n", va + (i << 12), pte[i] & 0xfffff000);
		}
	}
	return -EAGAIN;
}
module_init(pgdprobe_init);
MODULE_LICENSE("GPL");
