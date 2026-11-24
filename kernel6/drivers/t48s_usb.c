// SPDX-License-Identifier: GPL-2.0
/*
 * Yealink T48S: the USB socket, as a host (issues #33 and #60,
 * docs/installer.md).
 *
 * The SoC has two USB controllers and one PHY block (Linux 3.6 mach-bcmring:
 * MM_ADDR_IO_USB_CTLR0 0x80400000, CTLR1 0x80410000, USB_PHY 0x80420000). The
 * phone's socket is on controller 1, whose EHCI registers start at 0x80410800
 * (read from U-Boot 2.5.3.0-T48S: ehci_hcd_init at 0x03B24EC4 hands out
 * 0x80400800 or 0x80410800, and the phone uses index 1). The EHCI itself is
 * standard and is driven by the kernel's own ehci-platform driver
 * ("generic-ehci").
 *
 * Its interrupt is line 17 of the first interrupt controller, IRQ_USBHD2
 * ("USB host2/device2"), level, active high. Not 18: IRQ_USBH1 belongs to
 * controller 0, which this phone does not use. Yealink's 2.6.27 kernel has
 * the pair as a table, { 18, 17 }, indexed by controller
 * (bcmring_UsbEhci_ModuleInit), and on the phone the raw status of line 17
 * follows the controller's own status register (issue #60).
 *
 * The block is started here if nobody has: clocks, the PHY and two pins.
 * U-Boot does that only when it starts the phone from a USB stick (its
 * routine at 0x03B24CD0); on any other start (flash, TFTP) the block is off,
 * and reading the PHY or the controller with the bus clock off would stop the
 * kernel, as the audio block did (docs/audio.md). So chip control is looked at
 * first. If the USB host's bus clock, the USB clock and host mode are all on,
 * U-Boot has done it and nothing is written. Otherwise t48s_usb_start() below
 * does what U-Boot's routine does, in its order, with longer waits; the same
 * writes were first made by hand from user space on a phone started from
 * flash, and the controller came up (issue #60).
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define CHIPC_PHYS		0x80000000
#define CHIPC_USBCLK		0x50		/* PLL2 channel 1 */
#define  CHIPC_CLK_MDIV		0x000000ff
#define  CHIPC_CLK_MDIV_USB	60		/* 1800 MHz / 60 = 30 MHz */
#define  CHIPC_CLK_OUTPUT	0x00010000
#define  CHIPC_CLK_OFF		0x00080000
#define CHIPC_BUSCLK		0x5c
#define  CHIPC_BUSCLK_USBH	0x10000000
#define  CHIPC_BUSCLK_USBD	0x20000000
#define CHIPC_MISC		0x90
#define  CHIPC_MISC_USB_OFF	0x00000001
#define  CHIPC_MISC_USB_HOST	0x00000002
#define CHIPC_GPIOMUX(pin)	(0x118 + 4 * ((pin) / 8))	/* 4 bits per pin */
#define USB_PIN_FIRST		59
#define USB_PIN_LAST		60
#define USB_PIN_FUNCTION	13

/* the PHY block: no names are known, the values are U-Boot's */
#define PHY_PHYS		0x80420000
#define PHY_STATUS		0x0c
#define  PHY_STATUS_PLL_LOCK	0x01000000
#define PHY_PORTS		0x10
#define PHY_PLL			0x18
#define PHY_CTRL		0x24
#define PHY_TUNE		0x30

static void set_bits(void __iomem *reg, u32 bits)
{
	writel(readl(reg) | bits, reg);
}

static void clear_bits(void __iomem *reg, u32 bits)
{
	writel(readl(reg) & ~bits, reg);
}

static int t48s_usb_start(struct device *dev, void __iomem *chipc)
{
	void __iomem *phy;
	u32 v;
	int pin, port, ret;

	v = readl(chipc + CHIPC_USBCLK);
	if ((v & CHIPC_CLK_MDIV) != CHIPC_CLK_MDIV_USB) {
		dev_err(dev, "USB clock divider is %u, not %u: left alone\n",
			v & CHIPC_CLK_MDIV, CHIPC_CLK_MDIV_USB);
		return -ENODEV;
	}
	clear_bits(chipc + CHIPC_MISC, CHIPC_MISC_USB_OFF);
	set_bits(chipc + CHIPC_BUSCLK, CHIPC_BUSCLK_USBH);
	set_bits(chipc + CHIPC_BUSCLK, CHIPC_BUSCLK_USBD);
	clear_bits(chipc + CHIPC_USBCLK, CHIPC_CLK_OFF);
	set_bits(chipc + CHIPC_USBCLK, CHIPC_CLK_OUTPUT);
	usleep_range(1000, 2000);

	/* only now can the PHY block be read */
	phy = ioremap(PHY_PHYS, 0x100);
	if (!phy)
		return -ENOMEM;
	for (port = 0; port < 2; port++)
		set_bits(phy + PHY_PORTS, BIT(28 + port));
	for (port = 0; port < 2; port++)
		set_bits(phy + PHY_PORTS, BIT(30 + port));
	for (port = 0; port < 2; port++) {
		set_bits(phy + PHY_PORTS, BIT(21 + port));
		clear_bits(phy + PHY_PORTS, BIT(26 + port));
	}
	set_bits(phy + PHY_PORTS, BIT(19));
	clear_bits(phy + PHY_PLL, BIT(3));
	usleep_range(1000, 2000);
	set_bits(phy + PHY_PLL, BIT(3));
	usleep_range(1000, 2000);
	ret = readl_poll_timeout(phy + PHY_STATUS, v, v & PHY_STATUS_PLL_LOCK, 1000, 100000);
	if (ret) {
		dev_err(dev, "the USB PHY's PLL did not lock (status 0x%08x): no USB\n", v);
		goto out;
	}
	set_bits(phy + PHY_PLL, BIT(10));
	writel(0x2a, phy + PHY_CTRL);
	writel(5, phy + PHY_TUNE);
	usleep_range(1000, 2000);
	writel(0x2a, phy + PHY_CTRL);
	set_bits(phy + PHY_CTRL, 0x900);
	set_bits(phy + PHY_CTRL, BIT(22));
	set_bits(phy + PHY_CTRL, BIT(6));
	usleep_range(3000, 5000);
	clear_bits(phy + PHY_CTRL, BIT(6));

	clear_bits(chipc + CHIPC_MISC, CHIPC_MISC_USB_OFF);
	for (pin = USB_PIN_FIRST; pin <= USB_PIN_LAST; pin++) {
		v = readl(chipc + CHIPC_GPIOMUX(pin));
		v &= ~(0xf << (4 * (pin % 8)));
		v |= USB_PIN_FUNCTION << (4 * (pin % 8));
		writel(v, chipc + CHIPC_GPIOMUX(pin));
	}
	set_bits(chipc + CHIPC_MISC, CHIPC_MISC_USB_HOST);
	set_bits(phy + PHY_PORTS, BIT(27));
	clear_bits(phy + PHY_PORTS, BIT(22));
out:
	iounmap(phy);
	return ret;
}

static int t48s_usb_probe(struct platform_device *pdev)
{
	void __iomem *chipc = ioremap(CHIPC_PHYS, 0x1000);
	u32 clk, bus, misc;
	int ret = 0;

	if (!chipc)
		return -ENOMEM;
	clk = readl(chipc + CHIPC_USBCLK);
	bus = readl(chipc + CHIPC_BUSCLK);
	misc = readl(chipc + CHIPC_MISC);
	dev_info(&pdev->dev, "chip control as found: USB clock 0x%08x, bus clocks 0x%08x, misc 0x%08x\n",
		 clk, bus, misc);
	if (!(bus & CHIPC_BUSCLK_USBH) || !(clk & CHIPC_CLK_OUTPUT) || (clk & CHIPC_CLK_OFF) ||
	    (misc & CHIPC_MISC_USB_OFF) || !(misc & CHIPC_MISC_USB_HOST)) {
		ret = t48s_usb_start(&pdev->dev, chipc);
		if (!ret)
			dev_info(&pdev->dev, "USB was off: started as a host\n");
	} else {
		dev_info(&pdev->dev, "USB host is running, as U-Boot left it\n");
	}
	iounmap(chipc);
	if (ret)
		return ret;
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
MODULE_DESCRIPTION("Yealink T48S USB host");
