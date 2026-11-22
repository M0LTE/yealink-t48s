/*
 * t48s_fb: the Yealink SIP-T48S screen as a Linux framebuffer.
 *
 * U-Boot (and Yealink's kernel) leave the SoC's ARM PL111 LCD controller
 * running: 800x480, 32 bits per pixel (0x00RRGGBB in memory order B, G, R, X),
 * scanning out of a buffer just above the RAM Linux uses. This driver does not
 * touch the controller's timing; it reads where the controller is scanning
 * from (LCDUPBASE) and offers that memory as /dev/fb0, which also gives the
 * kernel a text console on the screen.
 *
 * GPL v2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/io.h>

#define DRV_NAME	"t48s_fb"
#define CLCD_PHYS	0x30500000
#define CLCD_UPBASE	0x10
#define CLCD_CONTROL	0x18
#define XRES		800
#define YRES		480
#define BPP		32
#define STRIDE		(XRES * BPP / 8)
#define FB_SIZE		(STRIDE * YRES)

static u32 t48s_pseudo_palette[16];

static int t48s_fb_setcolreg(unsigned regno, unsigned red, unsigned green,
			     unsigned blue, unsigned transp, struct fb_info *info)
{
	if (regno >= 16)
		return -EINVAL;
	/* 16-bit components in, 8-bit fields out */
	t48s_pseudo_palette[regno] = ((red >> 8) << 16) | ((green >> 8) << 8) | (blue >> 8);
	return 0;
}

static struct fb_ops t48s_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_setcolreg	= t48s_fb_setcolreg,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
};

static int __init t48s_fb_init(void)
{
	struct fb_info *info;
	void __iomem *clcd;
	u32 base, ctrl;
	int rc;

	clcd = ioremap(CLCD_PHYS, 0x1000);
	if (!clcd)
		return -ENOMEM;
	base = readl(clcd + CLCD_UPBASE);
	ctrl = readl(clcd + CLCD_CONTROL);
	iounmap(clcd);
	if (!(ctrl & 1) || !base) {
		pr_info(DRV_NAME ": LCD controller not running (control 0x%x), no screen\n", ctrl);
		return -ENODEV;
	}

	info = framebuffer_alloc(0, NULL);
	if (!info)
		return -ENOMEM;

	info->screen_base = ioremap_wc(base, FB_SIZE);
	if (!info->screen_base) {
		framebuffer_release(info);
		return -ENOMEM;
	}

	strlcpy(info->fix.id, "t48s", sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.smem_start = base;
	info->fix.smem_len = FB_SIZE;
	info->fix.line_length = STRIDE;
	info->fix.accel = FB_ACCEL_NONE;

	info->var.xres = info->var.xres_virtual = XRES;
	info->var.yres = info->var.yres_virtual = YRES;
	info->var.bits_per_pixel = BPP;
	info->var.red.offset = 16;
	info->var.red.length = 8;
	info->var.green.offset = 8;
	info->var.green.length = 8;
	info->var.blue.offset = 0;
	info->var.blue.length = 8;
	info->var.transp.offset = 24;
	info->var.transp.length = 0;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.height = -1;
	info->var.width = -1;
	info->var.vmode = FB_VMODE_NONINTERLACED;

	info->fbops = &t48s_fb_ops;
	info->flags = FBINFO_DEFAULT;
	info->pseudo_palette = t48s_pseudo_palette;

	rc = register_framebuffer(info);
	if (rc) {
		iounmap(info->screen_base);
		framebuffer_release(info);
		return rc;
	}
	pr_info("fb%d: T48S screen %dx%d at 0x%08x\n", info->node, XRES, YRES, base);
	return 0;
}
device_initcall(t48s_fb_init);
MODULE_LICENSE("GPL");
