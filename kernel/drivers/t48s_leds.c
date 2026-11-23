/* SPDX-License-Identifier: GPL-2.0 */
/*
 * t48s_leds: the four lamps of the Yealink SIP-T48S (hardware id 65), as
 * ordinary Linux LEDs under /sys/class/leds:
 *
 *   t48s:red:message    the lamp at the top right of the case: the message
 *                       and ringing lamp (Yealink's files call it "power").
 *                       It hangs off the Holtek chip (command 4), not a GPIO.
 *   t48s:green:headset  in the Headset key, GPIO3
 *   t48s:red:mute       in the Mute key, GPIO4
 *   t48s:green:speaker  in the Speakerphone key, GPIO5
 *
 * The three GPIO lamps are active low: 0 on the pin lights the lamp. There
 * are no other lamps on this phone (the Message key has none; the lamp at the
 * top right is the one Yealink blinks for ringing, missed calls and voicemail).
 *
 * This is what Yealink's keypad.ko does for this hardware id: its LED word
 * has bit 0 for the Holtek lamp and bits 1 to 3 for GPIO3 to 5, written
 * inverted, and its userspace (LedLamp.xml) names lamps 0 to 3 power,
 * headset, mute, speaker. Checked on the phone under Yealink: turning its
 * led1, led2, led3 on cleared bits 3, 4, 5 of the GPIO data register. See
 * docs/keypad-touch-hw.md.
 *
 * Blinking is the LED core's "timer" trigger:
 *   echo timer > trigger; echo 300 > delay_on; echo 300 > delay_off
 *
 * GPL v2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/leds.h>
#include <linux/workqueue.h>
#include <linux/t48s_holtek.h>

#define DRV_NAME	"t48s_leds"

#define CHIPC_PHYS	0x80000000
#define CHIPC_GPIOMUX0	0x118		/* GPIO0-7, 4 bits per pin, 0 = plain GPIO */
#define GPIO0_PHYS	0x80004000	/* GPIO0-31, one bit per pin */
#define GPIO_DATA	0x000
#define GPIO_DIR	0x400		/* 1 = output */

static void __iomem *gpio0;

/* these GPIO registers have no set/clear form: read, change, write, interrupts off */
static void gpio_change(void __iomem *reg, u32 bit, int on)
{
	unsigned long flags;
	u32 v;

	local_irq_save(flags);
	v = readl(reg);
	writel(on ? v | bit : v & ~bit, reg);
	local_irq_restore(flags);
}

struct t48s_gpio_led {
	struct led_classdev cdev;
	u32 bit;
};

static void gpio_led_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct t48s_gpio_led *led = container_of(cdev, struct t48s_gpio_led, cdev);

	gpio_change(gpio0 + GPIO_DATA, led->bit, value == LED_OFF);
}

/* the pin itself, so reading "brightness" shows what the hardware is doing */
static enum led_brightness gpio_led_get(struct led_classdev *cdev)
{
	struct t48s_gpio_led *led = container_of(cdev, struct t48s_gpio_led, cdev);

	return (readl(gpio0 + GPIO_DATA) & led->bit) ? LED_OFF : 1;
}

#define GPIO_LED(_name, _pin) {				\
	.cdev = {					\
		.name = _name,				\
		.max_brightness = 1,			\
		.brightness_set = gpio_led_set,		\
		.brightness_get = gpio_led_get,		\
	},						\
	.bit = 1u << (_pin),				\
}

static struct t48s_gpio_led gpio_leds[] = {
	GPIO_LED("t48s:green:headset", 3),
	GPIO_LED("t48s:red:mute", 4),
	GPIO_LED("t48s:green:speaker", 5),
};

/*
 * The message lamp: talking to the Holtek chip sleeps, and the LED core may
 * call us from a timer, so the chip is told from a work item. The chip has
 * no command to read the lamp back.
 */
static bool message_want;
static bool message_registered;

static void message_led_work(struct work_struct *work)
{
	t48s_holtek_set_led(message_want);
}
static DECLARE_WORK(message_work, message_led_work);

static void message_led_set(struct led_classdev *cdev, enum led_brightness value)
{
	message_want = value != LED_OFF;
	schedule_work(&message_work);
}

static struct led_classdev message_led = {
	.name = "t48s:red:message",
	.max_brightness = 1,
	.brightness_set = message_led_set,
};

static int __init t48s_leds_init(void)
{
	void __iomem *chipc;
	unsigned long flags;
	u32 bits = 0, v;
	int i, rc;

	gpio0 = ioremap(GPIO0_PHYS, 0x1000);
	chipc = ioremap(CHIPC_PHYS, 0x1000);
	if (!gpio0 || !chipc) {
		if (gpio0)
			iounmap(gpio0);
		if (chipc)
			iounmap(chipc);
		return -ENOMEM;
	}
	for (i = 0; i < ARRAY_SIZE(gpio_leds); i++)
		bits |= gpio_leds[i].bit;

	/* as Yealink's driver starts: plain GPIO, outputs, high (lamps off) */
	local_irq_save(flags);
	v = readl(chipc + CHIPC_GPIOMUX0);
	if (v & 0x00fff000)
		writel(v & ~0x00fff000, chipc + CHIPC_GPIOMUX0);
	local_irq_restore(flags);
	iounmap(chipc);
	gpio_change(gpio0 + GPIO_DATA, bits, 1);
	gpio_change(gpio0 + GPIO_DIR, bits, 1);

	for (i = 0; i < ARRAY_SIZE(gpio_leds); i++) {
		rc = led_classdev_register(NULL, &gpio_leds[i].cdev);
		if (rc)
			goto fail;
	}

	/* the lamp may have been left on by whatever ran before us */
	if (t48s_holtek_set_led(false) == 0) {
		rc = led_classdev_register(NULL, &message_led);
		if (rc)
			goto fail;
		message_registered = true;
	} else {
		pr_info(DRV_NAME ": no Holtek chip, so no message lamp\n");
	}
	pr_info(DRV_NAME ": %s on the Holtek chip; headset, mute, speaker on GPIO3, 4, 5\n",
		message_registered ? "message lamp" : "no message lamp");
	return 0;

fail:
	while (i--)
		led_classdev_unregister(&gpio_leds[i].cdev);
	iounmap(gpio0);
	return rc;
}
late_initcall(t48s_leds_init);
MODULE_LICENSE("GPL");
