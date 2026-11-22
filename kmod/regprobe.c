/* regprobe: print a few registers from kernel mode (insmod regprobe.ko base=0x32000000 offs=...). */
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/io.h>
static unsigned long base = 0x32000000;
static int offs[32] = {0x398, 0x3a0, 0x3a8, 0x3b0, 0x3f8, 0x0, 0x58, 0x5c};
static int noffs = 8;
module_param(base, ulong, 0);
module_param_array(offs, int, &noffs, 0);
static int __init regprobe_init(void)
{
	void __iomem *p = ioremap(base, 0x1000);
	int i;
	if (!p) return -ENOMEM;
	for (i = 0; i < noffs; i++)
		printk(KERN_INFO "regprobe: 0x%08lx = 0x%08x\n", base + offs[i], readl(p + offs[i]));
	iounmap(p);
	return -EAGAIN;	/* do not stay loaded */
}
module_init(regprobe_init);
MODULE_LICENSE("GPL");
