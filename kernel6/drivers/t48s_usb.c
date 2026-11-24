// SPDX-License-Identifier: GPL-2.0
/*
 * Yealink T48S: the USB socket, as a host, when U-Boot has already started it
 * (issue #33, docs/installer.md).
 *
 * The SoC has two USB controllers and one PHY block (Linux 3.6 mach-bcmring:
 * MM_ADDR_IO_USB_CTLR0 0x80400000, CTLR1 0x80410000, USB_PHY 0x80420000). The
 * phone's socket is on controller 1, whose EHCI registers start at 0x80410800
 * (read from U-Boot 2.5.3.0-T48S: ehci_hcd_init at 0x03B24EC4 hands out
 * 0x80400800 or 0x80410800, and the phone uses index 1). Its interrupt is
 * IRQ_USBH1, 18 on the first interrupt controller. The EHCI itself is standard
 * and is driven by the kernel's own ehci-platform driver ("generic-ehci").
 *
 * What is NOT here is the cold start: clocks, resets, the PHY and two pins.
 * U-Boot does all of that when it starts the phone from a USB stick (its
 * routine at 0x03B24CD0), and that is the one case the stick is needed in: the
 * installer reads its files from the stick the phone was started from and
 * writes the backup to it. On any other start (flash, TFTP, kexec) nobody has
 * switched the block on, and reading its registers would stop the kernel, as
 * the audio block did (docs/audio.md). So this driver looks at chip control
 * first, and only if the USB host's bus clock and the USB clock are both
 * running does it let the EHCI driver at the controller. Otherwise it says so
 * and there is no USB. Nothing in chip control or the PHY is written.
 *
 * What U-Boot's start does, for whoever writes the cold start (chip control at
 * 0x80000000, PHY block at 0x80420000): clear bit 0 of chip control 0x90; set
 * the USB PLL channel (clock 18); bus clocks 0x5c |= 0x10000000 | 0x20000000;
 * clock 0x50: clear bit 19, set bit 16; PHY 0x10: per port set bits 28+n,
 * 30+n, 21+n and clear 26+n, then set bit 19; PHY 0x18: bit 3 low, wait, high,
 * wait; wait for PHY 0x0c bit 24; PHY 0x18 |= 0x400; PHY 0x24 = 0x2a, PHY 0x30
 * = 5, PHY 0x24 = 0x2a, |= 0x900, |= 0x400000, |= 0x40, wait, clear 0x40;
 * chip control 0x90: clear bit 0, set bit 1 (host mode); pins 59 and 60 to
 * function 13; PHY 0x10: set bit 27, clear bit 22.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define CHIPC_PHYS		0x80000000
#define CHIPC_USBCLK		0x50
#define  CHIPC_CLK_OUTPUT	0x00010000
#define  CHIPC_CLK_OFF		0x00080000
#define CHIPC_BUSCLK		0x5c
#define  CHIPC_BUSCLK_USBH	0x10000000
#define CHIPC_MISC		0x90
#define  CHIPC_MISC_USB_OFF	0x00000001
#define  CHIPC_MISC_USB_HOST	0x00000002

static int t48s_usb_probe(struct platform_device *pdev)
{
	void __iomem *chipc = ioremap(CHIPC_PHYS, 0x1000);
	u32 clk, bus, misc;

	if (!chipc)
		return -ENOMEM;
	clk = readl(chipc + CHIPC_USBCLK);
	bus = readl(chipc + CHIPC_BUSCLK);
	misc = readl(chipc + CHIPC_MISC);
	iounmap(chipc);
	dev_info(&pdev->dev, "chip control as found: USB clock 0x%08x, bus clocks 0x%08x, misc 0x%08x\n",
		 clk, bus, misc);
	if (!(bus & CHIPC_BUSCLK_USBH) || !(clk & CHIPC_CLK_OUTPUT) || (clk & CHIPC_CLK_OFF) ||
	    (misc & CHIPC_MISC_USB_OFF) || !(misc & CHIPC_MISC_USB_HOST)) {
		dev_info(&pdev->dev, "USB is off (the phone was not started from a stick): no USB\n");
		return -ENODEV;
	}
	dev_info(&pdev->dev, "USB host is running, as U-Boot left it\n");
	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id t48s_usb_of_match[] = {
	{ .compatible = "brcm,bcm11107-usbh" },
	{ }
};

static struct platform_driver t48s_usb_driver = {
	.probe = t48s_usb_probe,
	.driver = {
		.name = "t48s_usb",
		.of_match_table = t48s_usb_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(t48s_usb_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Yealink T48S USB host, when U-Boot has started it");
