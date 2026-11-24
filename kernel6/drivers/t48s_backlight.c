/* SPDX-License-Identifier: GPL-2.0 */
/*
 * t48s_backlight: the screen backlight of the Yealink SIP-T48S (hardware id
 * 65), as an ordinary Linux backlight device, /sys/class/backlight/t48s.
 *
 *   brightness   0 to 10. 0 switches the lamp off; 1 is the dimmest Yealink's
 *                own firmware ever uses, 10 the brightest.
 *   bl_power     0 on, 4 off (the usual FB_BLANK numbers), without losing
 *                the brightness.
 *   actual_brightness   read back from the hardware, not remembered.
 *
 * Two things drive the lamp:
 *
 *   GPIO18   the enable. Low is on, high is off.
 *   PWM      one register of the SoC's LED/PWM block, 0x80002040, output on
 *            GPIO35 (pin function 13). Bits 0 to 2 are set once to start it;
 *            bits 3 to 7 are the level, and a smaller number is brighter.
 *
 * This is what Yealink's keypad.ko does for this hardware id (bklight_init,
 * bklight_init_pwm, bklight_set; see docs/keypad-touch-hw.md). It takes a
 * level of 0 to 99 and writes 30 - 3 * ceil(level / 10) to the PWM field, so
 * it only ever uses the ten values 27, 24, ... 0, and drives GPIO18 high for
 * level 0 without touching the PWM. We keep to those ten values, as nothing
 * else has been seen working: brightness n is PWM field 30 - 3n.
 *
 * U-Boot leaves the lamp on (GPIO18 low, PWM field 6, which is 8 here), so
 * loading this driver changes nothing on the screen: it starts from what the
 * hardware holds.
 *
 * Other T4xS models have the same PWM and another enable pin and curve. Both
 * come from the device tree node "yealink,t4xs-backlight":
 *   yealink,enable-pin    the enable's GPIO number, 0 to 31 (T48S 18, T46S 27)
 *   yealink,pwm-fields    ten PWM fields, for brightness 1 to 10
 * Without the node or a property, the T48S values above are used.
 *
 * GPL v2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/of.h>
#include <linux/backlight.h>

#define DRV_NAME	"t48s_backlight"

#define CHIPC_PHYS	0x80000000
#define CHIPC_LEDCLOCK	0x04c		/* bit 17: the PWM's clock */
#define CHIPC_BUSCLOCK	0x05c		/* bit 16: the LED/PWM block's bus clock */
#define CHIPC_GPIOMUX(pin)	(0x118 + 4 * ((pin) / 8))	/* 4 bits per pin, 0 = plain GPIO */
#define GPIOMUX_MASK(pin)	(0xfu << (((pin) % 8) * 4))
#define CHIPC_GPIOMUX4	0x128		/* GPIO32-39 */
#define MUX4_GPIO35	0x0000f000
#define MUX4_GPIO35_PWM	0x0000d000	/* function 13 */

#define GPIO0_PHYS	0x80004000	/* GPIO0-31, one bit per pin */
#define GPIO_DATA	0x000
#define GPIO_DIR	0x400		/* 1 = output */
#define ENABLE_PIN_T48S	18		/* low = lamp on */

#define PWM_PHYS	0x80002000
#define PWM_LCD		0x40
#define PWM_RUN		0x07		/* bits 0-2, as Yealink sets them */
#define PWM_LEVEL	0xf8		/* bits 3-7, smaller is brighter */
#define PWM_SHIFT	3

#define MAX_BRIGHTNESS	10

static void __iomem *gpio0, *pwm;
static u32 enable_bit = 1u << ENABLE_PIN_T48S;
/* PWM field for brightness 1 to 10; the T48S's, 30 - 3n */
static u8 pwm_field[MAX_BRIGHTNESS] = { 27, 24, 21, 18, 15, 12, 9, 6, 3, 0 };

/* none of these registers has a set/clear form: read, change, write, interrupts off */
static void reg_change(void __iomem *reg, u32 clear, u32 set)
{
	unsigned long flags;
	u32 v;

	local_irq_save(flags);
	v = readl(reg);
	writel((v & ~clear) | set, reg);
	local_irq_restore(flags);
}

static int field_to_brightness(u32 field)
{
	int i, best = 0;

	/* the nearest of the ten; of two equally near, the brighter (as 30 - 3n rounded before) */
	for (i = 1; i < MAX_BRIGHTNESS; i++)
		if (abs((int)pwm_field[i] - (int)field) <= abs((int)pwm_field[best] - (int)field))
			best = i;
	return best + 1;
}

/* what the hardware is doing now */
static int t48s_bl_get_brightness(struct backlight_device *bd)
{
	if (readl(gpio0 + GPIO_DATA) & enable_bit)
		return 0;
	return field_to_brightness((readl(pwm + PWM_LCD) & PWM_LEVEL) >> PWM_SHIFT);
}

static int t48s_bl_update_status(struct backlight_device *bd)
{
	int b = bd->props.brightness;

	if (backlight_is_blank(bd))
		b = 0;
	if (b <= 0) {
		/* off: the enable only, as Yealink does; the level stays for next time */
		reg_change(gpio0 + GPIO_DATA, 0, enable_bit);
		return 0;
	}
	if (b > MAX_BRIGHTNESS)
		b = MAX_BRIGHTNESS;
	/* the level first, so the lamp comes on at the right one */
	reg_change(pwm + PWM_LCD, PWM_LEVEL, pwm_field[b - 1] << PWM_SHIFT);
	reg_change(gpio0 + GPIO_DATA, enable_bit, 0);
	return 0;
}

static const struct backlight_ops t48s_bl_ops = {
	.update_status	= t48s_bl_update_status,
	.get_brightness	= t48s_bl_get_brightness,
};

static int __init t48s_backlight_init(void)
{
	struct backlight_properties props;
	struct backlight_device *bd;
	struct device_node *np;
	void __iomem *chipc;
	u32 pin = ENABLE_PIN_T48S;
	int lit, started;

	if (!of_machine_is_compatible("yealink,sip-t4xs"))
		return -ENODEV;
	np = of_find_compatible_node(NULL, NULL, "yealink,t4xs-backlight");
	if (np) {
		u8 fields[MAX_BRIGHTNESS];

		if (!of_property_read_u32(np, "yealink,enable-pin", &pin) && pin > 31) {
			pr_err(DRV_NAME ": enable pin %u is not in the first GPIO bank\n", pin);
			of_node_put(np);
			return -EINVAL;
		}
		if (!of_property_read_u8_array(np, "yealink,pwm-fields", fields, MAX_BRIGHTNESS))
			memcpy(pwm_field, fields, sizeof(pwm_field));
		of_node_put(np);
	}
	enable_bit = 1u << pin;
	chipc = ioremap(CHIPC_PHYS, 0x1000);
	gpio0 = ioremap(GPIO0_PHYS, 0x1000);
	pwm = ioremap(PWM_PHYS, 0x1000);
	if (!chipc || !gpio0 || !pwm) {
		if (chipc)
			iounmap(chipc);
		if (gpio0)
			iounmap(gpio0);
		if (pwm)
			iounmap(pwm);
		return -ENOMEM;
	}

	/*
	 * U-Boot has normally done all of this; only what is missing is
	 * written, so a lit screen stays exactly as it is.
	 */
	started = (readl(chipc + CHIPC_GPIOMUX4) & MUX4_GPIO35) == MUX4_GPIO35_PWM;
	if (!started) {
		reg_change(chipc + CHIPC_BUSCLOCK, 0, 1u << 16);
		reg_change(chipc + CHIPC_LEDCLOCK, 0, 1u << 17);
		reg_change(pwm + PWM_LCD, PWM_LEVEL, 0x06);
		reg_change(pwm + PWM_LCD, 0, 0x01);
		reg_change(chipc + CHIPC_GPIOMUX4, MUX4_GPIO35, MUX4_GPIO35_PWM);
	}
	/* the enable: plain GPIO, an output; its level is left alone */
	if (readl(chipc + CHIPC_GPIOMUX(pin)) & GPIOMUX_MASK(pin))
		reg_change(chipc + CHIPC_GPIOMUX(pin), GPIOMUX_MASK(pin), 0);
	if (!(readl(gpio0 + GPIO_DIR) & enable_bit))
		reg_change(gpio0 + GPIO_DIR, 0, enable_bit);
	iounmap(chipc);

	lit = !(readl(gpio0 + GPIO_DATA) & enable_bit);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = MAX_BRIGHTNESS;
	props.brightness = field_to_brightness((readl(pwm + PWM_LCD) & PWM_LEVEL) >> PWM_SHIFT);
	props.power = lit ? BACKLIGHT_POWER_ON : BACKLIGHT_POWER_OFF;
	bd = backlight_device_register("t48s", NULL, NULL, &t48s_bl_ops, &props);
	if (IS_ERR(bd)) {
		iounmap(gpio0);
		iounmap(pwm);
		return PTR_ERR(bd);
	}
	pr_info(DRV_NAME ": %s, brightness %d of %d (PWM register 0x%02x%s)\n",
		lit ? "lit" : "off", bd->props.brightness, MAX_BRIGHTNESS,
		readl(pwm + PWM_LCD), started ? "" : ", started here");
	return 0;
}
late_initcall(t48s_backlight_init);
MODULE_LICENSE("GPL");
