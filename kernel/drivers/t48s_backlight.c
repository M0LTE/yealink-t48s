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
 * GPL v2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/fb.h>
#include <linux/backlight.h>

#define DRV_NAME	"t48s_backlight"

#define CHIPC_PHYS	0x80000000
#define CHIPC_LEDCLOCK	0x04c		/* bit 17: the PWM's clock */
#define CHIPC_BUSCLOCK	0x05c		/* bit 16: the LED/PWM block's bus clock */
#define CHIPC_GPIOMUX2	0x120		/* GPIO16-23, 4 bits per pin, 0 = plain GPIO */
#define CHIPC_GPIOMUX4	0x128		/* GPIO32-39 */
#define MUX2_GPIO18	0x00000f00
#define MUX4_GPIO35	0x0000f000
#define MUX4_GPIO35_PWM	0x0000d000	/* function 13 */

#define GPIO0_PHYS	0x80004000	/* GPIO0-31, one bit per pin */
#define GPIO_DATA	0x000
#define GPIO_DIR	0x400		/* 1 = output */
#define GPIO18		(1u << 18)	/* low = lamp on */

#define PWM_PHYS	0x80002000
#define PWM_LCD		0x40
#define PWM_RUN		0x07		/* bits 0-2, as Yealink sets them */
#define PWM_LEVEL	0xf8		/* bits 3-7, smaller is brighter */
#define PWM_SHIFT	3

#define MAX_BRIGHTNESS	10

static void __iomem *gpio0, *pwm;

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
	int b = (30 - (int)field + 1) / 3;	/* 27 -> 1, 0 -> 10, and the nearest for anything between */

	return clamp(b, 1, MAX_BRIGHTNESS);
}

/* what the hardware is doing now */
static int t48s_bl_get_brightness(struct backlight_device *bd)
{
	if (readl(gpio0 + GPIO_DATA) & GPIO18)
		return 0;
	return field_to_brightness((readl(pwm + PWM_LCD) & PWM_LEVEL) >> PWM_SHIFT);
}

static int t48s_bl_update_status(struct backlight_device *bd)
{
	int b = bd->props.brightness;

	if (bd->props.power != FB_BLANK_UNBLANK || bd->props.fb_blank != FB_BLANK_UNBLANK)
		b = 0;
	if (b <= 0) {
		/* off: the enable only, as Yealink does; the level stays for next time */
		reg_change(gpio0 + GPIO_DATA, 0, GPIO18);
		return 0;
	}
	if (b > MAX_BRIGHTNESS)
		b = MAX_BRIGHTNESS;
	/* the level first, so the lamp comes on at the right one */
	reg_change(pwm + PWM_LCD, PWM_LEVEL, (30 - 3 * b) << PWM_SHIFT);
	reg_change(gpio0 + GPIO_DATA, GPIO18, 0);
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
	void __iomem *chipc;
	int lit, started;

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
	if (readl(chipc + CHIPC_GPIOMUX2) & MUX2_GPIO18)
		reg_change(chipc + CHIPC_GPIOMUX2, MUX2_GPIO18, 0);
	if (!(readl(gpio0 + GPIO_DIR) & GPIO18))
		reg_change(gpio0 + GPIO_DIR, 0, GPIO18);
	iounmap(chipc);

	lit = !(readl(gpio0 + GPIO_DATA) & GPIO18);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = MAX_BRIGHTNESS;
	props.brightness = field_to_brightness((readl(pwm + PWM_LCD) & PWM_LEVEL) >> PWM_SHIFT);
	props.power = lit ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;
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
