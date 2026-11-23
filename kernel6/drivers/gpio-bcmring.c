// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO banks of Broadcom's BCM11107 family (BCM11188 in the Yealink T48S).
 *
 * A bank is up to 32 pins with a data register at 0x000 and a direction
 * register at 0x400 (1 = output). There are no set or clear registers, so a
 * pin is changed by reading the register, changing the bit and writing it
 * back with interrupts off. That is also what the T48S drivers that still
 * reach these registers by themselves do (Holtek chip, backlight, touch chip
 * select, loudspeaker amplifier), which is why this driver keeps no copy of
 * the register: on this one-CPU system the two ways can then live side by
 * side. (The kernel's generic memory-mapped GPIO driver keeps a copy and
 * would write stale levels over theirs.)
 *
 * Which function a pin has is set in the chip control block (0x80000118 on,
 * 4 bits per pin) and is left as the boot loader set it. The banks' interrupt
 * registers are not driven yet.
 */
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define GPIO_DATA	0x000
#define GPIO_DIR	0x400		/* 1 = output */

struct bcmring_gpio {
	struct gpio_chip gc;
	void __iomem *base;
};

static void bcmring_gpio_change(void __iomem *reg, u32 bit, int on)
{
	unsigned long flags;
	u32 v;

	local_irq_save(flags);
	v = readl(reg);
	writel(on ? v | bit : v & ~bit, reg);
	local_irq_restore(flags);
}

static int bcmring_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct bcmring_gpio *g = gpiochip_get_data(gc);

	return !!(readl(g->base + GPIO_DATA) & BIT(offset));
}

static int bcmring_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct bcmring_gpio *g = gpiochip_get_data(gc);

	bcmring_gpio_change(g->base + GPIO_DATA, BIT(offset), value);
	return 0;
}

static int bcmring_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct bcmring_gpio *g = gpiochip_get_data(gc);

	return readl(g->base + GPIO_DIR) & BIT(offset) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int bcmring_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct bcmring_gpio *g = gpiochip_get_data(gc);

	bcmring_gpio_change(g->base + GPIO_DIR, BIT(offset), 0);
	return 0;
}

static int bcmring_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
					 int value)
{
	struct bcmring_gpio *g = gpiochip_get_data(gc);

	/* the level first, so the pin starts driving at the right one */
	bcmring_gpio_change(g->base + GPIO_DATA, BIT(offset), value);
	bcmring_gpio_change(g->base + GPIO_DIR, BIT(offset), 1);
	return 0;
}

static int bcmring_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bcmring_gpio *g;
	u32 ngpios = 32;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	g->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(g->base))
		return PTR_ERR(g->base);
	device_property_read_u32(dev, "ngpios", &ngpios);
	if (!ngpios || ngpios > 32)
		return -EINVAL;

	g->gc.label = dev_name(dev);
	g->gc.parent = dev;
	g->gc.owner = THIS_MODULE;
	g->gc.base = -1;
	g->gc.ngpio = ngpios;
	g->gc.get = bcmring_gpio_get;
	g->gc.set = bcmring_gpio_set;
	g->gc.get_direction = bcmring_gpio_get_direction;
	g->gc.direction_input = bcmring_gpio_direction_input;
	g->gc.direction_output = bcmring_gpio_direction_output;
	return devm_gpiochip_add_data(dev, &g->gc, g);
}

static const struct of_device_id bcmring_gpio_of_match[] = {
	{ .compatible = "brcm,bcm11107-gpio" },
	{ }
};

static struct platform_driver bcmring_gpio_driver = {
	.probe = bcmring_gpio_probe,
	.driver = {
		.name = "gpio-bcmring",
		.of_match_table = bcmring_gpio_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init bcmring_gpio_init(void)
{
	return platform_driver_register(&bcmring_gpio_driver);
}
subsys_initcall(bcmring_gpio_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GPIO banks of the Broadcom BCM11107 family");
