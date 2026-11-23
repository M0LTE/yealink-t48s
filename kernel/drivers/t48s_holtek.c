/* SPDX-License-Identifier: GPL-2.0 */
/*
 * t48s_holtek: the Holtek companion chip of the Yealink SIP-T48S.
 *
 * The chip holds a watchdog that resets the phone (Yealink's software arms it
 * with 11 seconds and keeps feeding it), one LED (the power lamp, driven for
 * t48s_leds.c through t48s_holtek_set_led()), and the hardware version.
 * It hangs off three GPIOs, bit-banged: GPIO36 chip select (active low),
 * GPIO11 clock (idle high), GPIO10 data (one pin, both directions).
 *
 * All this driver does: read the version block at boot, and if the chip
 * answers as expected, turn its watchdog off. After a kexec from Yealink the
 * loader has already done that; this covers any boot path that has not.
 * The watchdog can be set by hand, in seconds, 0 = off:
 *   echo 30 > /sys/module/t48s_holtek/parameters/watchdog
 * Nothing in this kernel feeds it: writing N again restarts the count, and
 * if nothing does, the phone resets after N seconds.
 *
 * Protocol (version 2.0, from Yealink's keypad.ko, see docs/keypad-touch-hw.md):
 * chip select low, 4 command bits, then data bits, most significant first.
 * The chip takes our bits on the rising clock edge and shifts its own out on
 * the falling edge. About 6000 bits per second.
 *
 * GPL v2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/irqflags.h>
#include <linux/moduleparam.h>
#include <linux/t48s_holtek.h>

#define DRV_NAME	"t48s_holtek"

#define GPIO0_PHYS	0x80004000	/* GPIO0-31 */
#define GPIO1_PHYS	0x80004800	/* GPIO32-60 */
#define GPIO_DATA	0x000
#define GPIO_DIR	0x400		/* 1 = output */

#define SDA_BIT		(1u << 10)	/* GPIO10, bank 0 */
#define CLK_BIT		(1u << 11)	/* GPIO11, bank 0 */
#define CS_BIT		(1u << 4)	/* GPIO36, bank 1 */

#define HALF_BIT_US	83
#define CS_DELAY_US	500

#define CMD_BITS	4
#define CMD_WATCHDOG	3		/* write 8 bits: seconds, 0 = off */
#define CMD_LED		4		/* write 32 bits: 1 = the power lamp on, 0 = off */
#define CMD_VERSION	14		/* read 64 bits */

static void __iomem *gpio0;
static void __iomem *gpio1;
static DEFINE_MUTEX(holtek_lock);
static bool holtek_ok;
static unsigned int watchdog;	/* last value written, seconds */

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

static void sda_set(int v) { gpio_change(gpio0 + GPIO_DATA, SDA_BIT, v); }
static void clk_set(int v) { gpio_change(gpio0 + GPIO_DATA, CLK_BIT, v); }
static void cs_set(int v) { gpio_change(gpio1 + GPIO_DATA, CS_BIT, v); }
static void sda_dir(int out) { gpio_change(gpio0 + GPIO_DIR, SDA_BIT, out); }
static int sda_get(void) { return !!(readl(gpio0 + GPIO_DATA) & SDA_BIT); }

static void holtek_send_bits(u32 val, int bits)
{
	while (bits--) {
		sda_set((val >> bits) & 1);
		udelay(HALF_BIT_US);
		clk_set(0);
		udelay(HALF_BIT_US);
		clk_set(1);
	}
}

static void holtek_begin(int cmd)
{
	sda_dir(1);
	cs_set(0);
	udelay(CS_DELAY_US);
	holtek_send_bits(cmd, CMD_BITS);
}

static void holtek_end(void)
{
	udelay(HALF_BIT_US);
	sda_set(1);
	sda_dir(1);
	cs_set(1);
	udelay(CS_DELAY_US);
}

static void holtek_write(int cmd, u32 val, int bits)
{
	mutex_lock(&holtek_lock);
	holtek_begin(cmd);
	holtek_send_bits(val, bits);
	holtek_end();
	mutex_unlock(&holtek_lock);
}

static u64 holtek_read(int cmd, int bits)
{
	u64 val = 0;

	mutex_lock(&holtek_lock);
	holtek_begin(cmd);
	sda_dir(0);
	udelay(HALF_BIT_US);
	while (bits--) {
		clk_set(0);
		udelay(HALF_BIT_US);
		val = (val << 1) | sda_get();
		clk_set(1);
		udelay(HALF_BIT_US);
	}
	holtek_end();
	mutex_unlock(&holtek_lock);
	return val;
}

/* the chip's one LED, for t48s_leds */
int t48s_holtek_set_led(bool on)
{
	if (!holtek_ok)
		return -ENODEV;
	holtek_write(CMD_LED, on ? 1 : 0, 32);
	return 0;
}
EXPORT_SYMBOL_GPL(t48s_holtek_set_led);

static int watchdog_param_set(const char *val, const struct kernel_param *kp)
{
	unsigned int secs;
	int rc;

	rc = kstrtouint(val, 0, &secs);
	if (rc)
		return rc;
	if (secs > 255)
		return -EINVAL;
	if (!holtek_ok)
		return -ENODEV;
	holtek_write(CMD_WATCHDOG, secs, 8);
	watchdog = secs;
	return 0;
}

static struct kernel_param_ops watchdog_param_ops = {
	.set = watchdog_param_set,
	.get = param_get_uint,
};
module_param_cb(watchdog, &watchdog_param_ops, &watchdog, 0644);
MODULE_PARM_DESC(watchdog, "Holtek watchdog in seconds, 0 = off (last value written)");

static int __init t48s_holtek_init(void)
{
	u64 ver;
	u8 b[8];
	int i;

	gpio0 = ioremap(GPIO0_PHYS, 0x1000);
	gpio1 = ioremap(GPIO1_PHYS, 0x1000);
	if (!gpio0 || !gpio1) {
		if (gpio0)
			iounmap(gpio0);
		if (gpio1)
			iounmap(gpio1);
		return -ENOMEM;
	}

	/* as Yealink's driver starts: all three pins outputs at 1, one empty select pulse */
	mutex_lock(&holtek_lock);
	cs_set(1);
	clk_set(1);
	sda_set(1);
	gpio_change(gpio1 + GPIO_DIR, CS_BIT, 1);
	gpio_change(gpio0 + GPIO_DIR, CLK_BIT, 1);
	sda_dir(1);
	cs_set(0);
	udelay(CS_DELAY_US);
	cs_set(1);
	udelay(CS_DELAY_US);
	mutex_unlock(&holtek_lock);

	/* the first bit on the wire is the top bit of byte 7 */
	ver = holtek_read(CMD_VERSION, 64);
	for (i = 0; i < 8; i++)
		b[i] = ver >> (8 * i);
	/* a protocol 2.0 chip has 2 in byte 6; the hardware id is byte 5 */
	if (b[6] != 2) {
		pr_info(DRV_NAME ": no answer we understand (read 0x%016llx), leaving the chip alone\n",
			(unsigned long long)ver);
		return 0;
	}
	holtek_ok = true;
	holtek_write(CMD_WATCHDOG, 0, 8);
	pr_info(DRV_NAME ": hardware %u.%u.%u.%u.%u.%u.%u, protocol %u.%u, watchdog turned off\n",
		b[5], (u8)((b[4] << 4) | (b[4] >> 4)), b[7], b[3], b[2], b[1], b[0],
		b[6], b[7]);
	return 0;
}
device_initcall(t48s_holtek_init);
MODULE_LICENSE("GPL");
