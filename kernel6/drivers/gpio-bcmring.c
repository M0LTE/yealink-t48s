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
 * Each bank has one interrupt line into the second interrupt controller, and
 * per-pin interrupt registers laid out like an ARM PL061's, 32 bits wide:
 * 0x404 level (1) or edge (0), 0x408 both edges, 0x40C high or rising,
 * 0x410 enable, 0x418 status, 0x41C clear.
 *
 * Which function a pin has is set in the chip control block (0x80000118 on,
 * 4 bits per pin) and is left as the boot loader set it.
 */
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define GPIO_DATA	0x000
#define GPIO_DIR	0x400		/* 1 = output */
#define GPIO_IS		0x404		/* 1 = level, 0 = edge */
#define GPIO_IBE	0x408		/* both edges */
#define GPIO_IEV	0x40c		/* 1 = high or rising */
#define GPIO_IE		0x410		/* interrupt enable */
#define GPIO_MIS	0x418		/* interrupt status */
#define GPIO_IC		0x41c		/* interrupt clear, write 1 */

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

static int bcmring_gpio_irq_type(struct irq_data *d, unsigned int trigger)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct bcmring_gpio *g = gpiochip_get_data(gc);
	u32 bit = BIT(irqd_to_hwirq(d));

	switch (trigger & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_LEVEL_HIGH:
	case IRQ_TYPE_LEVEL_LOW:
		bcmring_gpio_change(g->base + GPIO_IBE, bit, 0);
		bcmring_gpio_change(g->base + GPIO_IEV, bit, trigger & IRQ_TYPE_LEVEL_HIGH);
		bcmring_gpio_change(g->base + GPIO_IS, bit, 1);
		irq_set_handler_locked(d, handle_level_irq);
		break;
	case IRQ_TYPE_EDGE_BOTH:
		bcmring_gpio_change(g->base + GPIO_IS, bit, 0);
		bcmring_gpio_change(g->base + GPIO_IBE, bit, 1);
		irq_set_handler_locked(d, handle_edge_irq);
		break;
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_EDGE_FALLING:
		bcmring_gpio_change(g->base + GPIO_IS, bit, 0);
		bcmring_gpio_change(g->base + GPIO_IBE, bit, 0);
		bcmring_gpio_change(g->base + GPIO_IEV, bit, trigger & IRQ_TYPE_EDGE_RISING);
		irq_set_handler_locked(d, handle_edge_irq);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void bcmring_gpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct bcmring_gpio *g = gpiochip_get_data(gc);
	struct irq_chip *parent = irq_desc_get_chip(desc);
	unsigned long pending;
	int offset;

	chained_irq_enter(parent, desc);
	pending = readl(g->base + GPIO_MIS);
	for_each_set_bit(offset, &pending, gc->ngpio)
		generic_handle_domain_irq(gc->irq.domain, offset);
	chained_irq_exit(parent, desc);
}

static void bcmring_gpio_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct bcmring_gpio *g = gpiochip_get_data(gc);

	bcmring_gpio_change(g->base + GPIO_IE, BIT(irqd_to_hwirq(d)), 0);
	gpiochip_disable_irq(gc, irqd_to_hwirq(d));
}

static void bcmring_gpio_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct bcmring_gpio *g = gpiochip_get_data(gc);

	gpiochip_enable_irq(gc, irqd_to_hwirq(d));
	bcmring_gpio_change(g->base + GPIO_IE, BIT(irqd_to_hwirq(d)), 1);
}

static void bcmring_gpio_irq_ack(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct bcmring_gpio *g = gpiochip_get_data(gc);

	writel(BIT(irqd_to_hwirq(d)), g->base + GPIO_IC);
}

static const struct irq_chip bcmring_gpio_irqchip = {
	.name = "bcmring-gpio",
	.irq_ack = bcmring_gpio_irq_ack,
	.irq_mask = bcmring_gpio_irq_mask,
	.irq_unmask = bcmring_gpio_irq_unmask,
	.irq_set_type = bcmring_gpio_irq_type,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int bcmring_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_irq_chip *girq;
	struct bcmring_gpio *g;
	u32 ngpios = 32;
	int irq;

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

	/* interrupts, if the device tree gives the bank's line; all off to start with */
	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		writel(0, g->base + GPIO_IE);
		writel(~0u, g->base + GPIO_IC);
		girq = &g->gc.irq;
		gpio_irq_chip_set_chip(girq, &bcmring_gpio_irqchip);
		girq->parent_handler = bcmring_gpio_irq_handler;
		girq->num_parents = 1;
		girq->parents = devm_kcalloc(dev, 1, sizeof(*girq->parents), GFP_KERNEL);
		if (!girq->parents)
			return -ENOMEM;
		girq->parents[0] = irq;
		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_bad_irq;
	}
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
