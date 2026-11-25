/* SPDX-License-Identifier: GPL-2.0 */
/*
 * t48s_keypad: the keys and the hook switch of the Yealink SIP-T48S.
 *
 * Keys: the SoC's own keypad controller (KEYC) at 0x8000D000 scans a matrix
 * on GPIO43-58 and keeps the result in five status words, two columns of nine
 * row bits per word, a 0 bit meaning pressed. U-Boot (and Yealink's kernel)
 * leave it running, so normally there is nothing to set up; if it is found
 * off it is started here.
 *   echo 1 > /sys/module/t48s_keypad/parameters/cold
 * tests that: it resets the controller, gates its bus clock and gives its
 * pins back, then starts it again.
 *
 * Hook: plain input GPIO29.
 *
 * Both are polled every 20 ms. The controller has an interrupt (IRQ 48) and
 * the GPIO bank has one too (IRQ 32), but how the keypad interrupt is
 * acknowledged is a guess so far, and this kernel has no GPIO interrupt
 * demultiplexer. Polling six registers 50 times a second costs nothing.
 *
 * Key positions are numbered the way Yealink's driver numbers them,
 * code = row * 8 + column + 1; that number is also sent as MSC_SCAN and is the
 * index for EVIOCGKEYCODE/EVIOCSKEYCODE. The table below comes from Yealink's
 * own Qt keymap for this phone (/phone/resource/device-res/keymap/T48.qmap),
 * which agrees with the key table in U-Boot. See docs/keypad-touch-hw.md.
 *
 * Other T4xS models have the same controller and matrix size with the keys in
 * other places. Their table comes from the device tree: the node
 * "yealink,t4xs-keypad" may hold "yealink,keymap", pairs of <Yealink code,
 * Linux key code>, which then replaces the T48S table below entirely.
 *
 * A position that is not in the table is sent as KEY_UNKNOWN, so that a
 * program reading MSC_SCAN sees every key there is: the shell's key test
 * (shell/keytest.c) shows row, column and code on the screen, which is how a
 * table is checked on a phone with no console.
 *
 * To check a key by hand:
 *   echo 1 > /sys/module/t48s_keypad/parameters/debug
 * then every press and release is logged with its row, column and codes.
 *
 * GPL v2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/irqflags.h>
#include <linux/of.h>

#define DRV_NAME	"t48s_keypad"

#define CHIPC_PHYS	0x80000000
#define CHIPC_BUSCLK	0x5c		/* bus clock gates */
#define CHIPC_BUSCLK_KEYC	0x8000
#define CHIPC_GPIOMUX(pin)	(0x118 + 4 * ((pin) / 8))	/* 4 bits per pin */
#define GPIOMUX_KEYPAD	1

#define GPIO0_PHYS	0x80004000	/* GPIO0-31, one bit per pin */
#define GPIO_DATA	0x000
#define HOOK_BIT	(1u << 29)
/*
 * GPIO29 read 1 with Yealink reporting hook_sta=1 while the handset was (as
 * far as we know) on its cradle. So: high = on hook. Not yet confirmed by
 * lifting the handset.
 */
#define HOOK_OFF_LEVEL	0

#define KEYC_PHYS	0x8000d000
#define KEYC_STATUS(i)	((i) * 4)	/* 0x00..0x10 */
#define KEYC_PRESCALE	0x14
#define KEYC_REG18	0x18		/* Yealink writes 200; debounce or scan period */
#define KEYC_CTRL	0x1c
#define KEYC_CTRL_EN	0x01
#define KEYC_CTRL_IRQEN	0x02
#define KEYC_CTRL_BIT3	0x08		/* cleared by Yealink, meaning unknown */
#define KEYC_CTRL_BIT4	0x10		/* set by Yealink, meaning unknown */
/* values Yealink's kernel left in the registers (prescaler = bus clock / 81000 - 1) */
#define KEYC_PRESCALE_VAL	0x808
#define KEYC_REG18_VAL		200
#define KEYC_FIRST_PIN	43
#define KEYC_LAST_PIN	58

#define ROWS		9		/* bits in a column */
#define COLS		7		/* columns in use on this phone */
#define NCODES		(ROWS * 8 + 1)	/* Yealink codes run from 1 to 71 */
#define CODE(row, col)	((row) * 8 + (col) + 1)

#define POLL_JIFFIES	msecs_to_jiffies(20)

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "log every key press and release with its matrix position");

/*
 * Index: Yealink's code for the matrix position. The digits, star, hash, the
 * arrows, OK, X and Speakerphone are known. The other function keys are sent
 * as the same F-key Yealink's keymap gives them (Qt::Key_F7 and so on), until
 * someone presses them and reads the labels.
 */
static unsigned short t48s_keymap[NCODES] = {
	[65] = KEY_1,		[66] = KEY_2,		[67] = KEY_3,
	[57] = KEY_4,		[58] = KEY_5,		[59] = KEY_6,
	[49] = KEY_7,		[50] = KEY_8,		[51] = KEY_9,
	[41] = KEY_KPASTERISK,	[42] = KEY_0,		[43] = KEY_NUMERIC_POUND,

	[68] = KEY_UP,		[52] = KEY_DOWN,
	[61] = KEY_LEFT,	[69] = KEY_RIGHT,
	[60] = KEY_ENTER,	/* OK */
	[53] = KEY_BACKSPACE,	/* X (Yealink: F13) */

	/*
	 * Phone keys, confirmed by pressing them (2026-11-22). Codes chosen so the text
	 * console ignores them (no escape sequences at the login prompt) and an
	 * application can tell them apart.
	 */
	[45] = KEY_VOLUMEDOWN,
	[44] = KEY_VOLUMEUP,
	[33] = KEY_PROG1,	/* Headset (has an LED) */
	[25] = KEY_MUTE,	/* Mute (has an LED) */
	[34] = KEY_MESSENGER,	/* Message */
	[26] = KEY_PAUSE,	/* Hold */
	[35] = KEY_AGAIN,	/* Redial */
	[37] = KEY_FORWARD,	/* Transfer */
	[36] = KEY_PROG2,	/* Speakerphone */
};

static void __iomem *keyc;
static void __iomem *gpio0;
static struct input_dev *keys_dev;
static struct input_dev *hook_dev;
static struct timer_list poll_timer;
static u16 last_col[COLS];	/* 1 bit = pressed */
static int hook_level = -1;	/* last reported GPIO level */
static int hook_seen = -1;	/* level at the previous poll, for debounce */

/*
 * Test hook: echo "CODE VALUE" > /sys/module/t48s_keypad/parameters/inject
 * reports a Linux key code (VALUE 1 down, 0 up) as if the key had been
 * pressed: KEY_PHONE (169) on the hook switch device, anything in the key
 * table on the keypad. It goes through the real input devices, so it reaches
 * whatever has them open (and grabbed), which writing to /dev/input/eventN
 * from another program does not when a device is grabbed.
 */
static int inject_set(const char *val, const struct kernel_param *kp)
{
	unsigned int code, value;
	struct input_dev *dev;

	if (sscanf(val, "%u %u", &code, &value) != 2 || code > KEY_MAX || value > 1)
		return -EINVAL;
	dev = code == KEY_PHONE ? hook_dev : keys_dev;
	if (!dev || !test_bit(code, dev->keybit))
		return -EINVAL;
	input_report_key(dev, code, value);
	input_sync(dev);
	return 0;
}

static const struct kernel_param_ops inject_ops = { .set = inject_set };
module_param_cb(inject, &inject_ops, NULL, 0200);
MODULE_PARM_DESC(inject, "\"CODE VALUE\": report a key (or KEY_PHONE, the hook) as pressed (1) or released (0), for tests");

static void t48s_keypad_read(u16 *col)
{
	u32 ctrl, w[5];
	int i;

	/* as Yealink's handler does: interrupt enable off, read, back on */
	ctrl = readl(keyc + KEYC_CTRL);
	writel(ctrl & ~KEYC_CTRL_IRQEN, keyc + KEYC_CTRL);
	for (i = 0; i < 5; i++)
		w[i] = readl(keyc + KEYC_STATUS(i));
	writel(ctrl, keyc + KEYC_CTRL);

	/* column 2i is bits 16-24 of word i, column 2i+1 is bits 0-8; 0 = pressed */
	for (i = 0; i < COLS; i++)
		col[i] = ~(i & 1 ? w[i / 2] : w[i / 2] >> 16) & 0x1ff;
}

static void t48s_keypad_poll(struct timer_list *unused)
{
	u16 col[COLS];
	int c, r, level;
	bool sync = false;

	t48s_keypad_read(col);
	for (c = 0; c < COLS; c++) {
		u16 changed = col[c] ^ last_col[c];

		for (r = 0; changed && r < ROWS; r++) {
			int code, key, down;

			if (!(changed & (1 << r)))
				continue;
			changed &= ~(1 << r);
			code = CODE(r, c);
			key = t48s_keymap[code];
			down = !!(col[c] & (1 << r));
			if (debug || key == KEY_RESERVED)
				pr_info(DRV_NAME ": row %d col %d code %d %s -> key %d\n",
					r, c, code, down ? "down" : "up", key);
			input_event(keys_dev, EV_MSC, MSC_SCAN, code);
			input_report_key(keys_dev, key != KEY_RESERVED ? key : KEY_UNKNOWN, down);
			sync = true;
		}
		last_col[c] = col[c];
	}
	if (sync)
		input_sync(keys_dev);

	/* hook: act on a level seen at two polls in a row */
	level = !!(readl(gpio0 + GPIO_DATA) & HOOK_BIT);
	if (level == hook_seen && level != hook_level) {
		hook_level = level;
		if (debug)
			pr_info(DRV_NAME ": hook GPIO29 = %d (%s hook)\n", level,
				level == HOOK_OFF_LEVEL ? "off" : "on");
		input_report_key(hook_dev, KEY_PHONE, level == HOOK_OFF_LEVEL);
		input_sync(hook_dev);
	}
	hook_seen = level;

	mod_timer(&poll_timer, jiffies + POLL_JIFFIES);
}

/*
 * Start the controller from nothing, in the order Yealink's driver and U-Boot
 * do it. Not needed after U-Boot or a kexec from Yealink (it is found
 * running); here for a boot path that skips both. Exercised by the "cold"
 * test hook below.
 */
static int t48s_keypad_start(void)
{
	void __iomem *chipc;
	unsigned long flags;
	u32 v;
	int pin;

	chipc = ioremap(CHIPC_PHYS, 0x1000);
	if (!chipc)
		return -ENOMEM;
	local_irq_save(flags);
	for (pin = KEYC_FIRST_PIN; pin <= KEYC_LAST_PIN; pin++) {
		v = readl(chipc + CHIPC_GPIOMUX(pin));
		v &= ~(0xfu << ((pin % 8) * 4));
		v |= GPIOMUX_KEYPAD << ((pin % 8) * 4);
		writel(v, chipc + CHIPC_GPIOMUX(pin));
	}
	writel(readl(chipc + CHIPC_BUSCLK) | CHIPC_BUSCLK_KEYC, chipc + CHIPC_BUSCLK);
	local_irq_restore(flags);
	iounmap(chipc);

	v = readl(keyc + KEYC_CTRL) & ~(KEYC_CTRL_EN | KEYC_CTRL_BIT3);
	writel(v, keyc + KEYC_CTRL);
	v |= KEYC_CTRL_BIT4;
	writel(v, keyc + KEYC_CTRL);
	writel(KEYC_REG18_VAL, keyc + KEYC_REG18);
	writel(KEYC_PRESCALE_VAL, keyc + KEYC_PRESCALE);
	v |= KEYC_CTRL_EN;	/* its interrupt stays off: we poll */
	writel(v, keyc + KEYC_CTRL);
	msleep(50);	/* let it scan once before the first read */
	return 0;
}

/* Test of the start above with no cold boot: reset the controller, gate its
 * bus clock, make its pins plain GPIO, then start it again. Polling is held
 * off meanwhile (a register of a block without its clock does not answer). */
#define CHIPC_SOFTRESET1	0x80
#define CHIPC_SOFTRESET1_KEYC	0x08000000

static int cold_set(const char *val, const struct kernel_param *kp)
{
	void __iomem *chipc;
	unsigned long flags;
	u32 v;
	int pin, rc;

	if (!keys_dev || val[0] != '1')
		return -EINVAL;
	chipc = ioremap(CHIPC_PHYS, 0x1000);
	if (!chipc)
		return -ENOMEM;
	timer_delete_sync(&poll_timer);
	local_irq_save(flags);
	v = readl(chipc + CHIPC_SOFTRESET1);
	writel(v | CHIPC_SOFTRESET1_KEYC, chipc + CHIPC_SOFTRESET1);
	udelay(100);
	writel(v & ~CHIPC_SOFTRESET1_KEYC, chipc + CHIPC_SOFTRESET1);
	pr_info(DRV_NAME ": test: after the reset control 0x%x, prescaler 0x%x\n",
		readl(keyc + KEYC_CTRL), readl(keyc + KEYC_PRESCALE));
	writel(readl(chipc + CHIPC_BUSCLK) & ~CHIPC_BUSCLK_KEYC, chipc + CHIPC_BUSCLK);
	for (pin = KEYC_FIRST_PIN; pin <= KEYC_LAST_PIN; pin++) {
		v = readl(chipc + CHIPC_GPIOMUX(pin));
		v &= ~(0xfu << ((pin % 8) * 4));
		writel(v, chipc + CHIPC_GPIOMUX(pin));
	}
	local_irq_restore(flags);
	iounmap(chipc);
	msleep(20);
	rc = t48s_keypad_start();
	pr_info(DRV_NAME ": test: started again (%d): control 0x%x, prescaler 0x%x, columns %03x %03x %03x %03x %03x\n",
		rc, readl(keyc + KEYC_CTRL), readl(keyc + KEYC_PRESCALE),
		readl(keyc + KEYC_STATUS(0)), readl(keyc + KEYC_STATUS(1)), readl(keyc + KEYC_STATUS(2)),
		readl(keyc + KEYC_STATUS(3)), readl(keyc + KEYC_STATUS(4)));
	mod_timer(&poll_timer, jiffies + POLL_JIFFIES);
	return rc;
}

static const struct kernel_param_ops cold_ops = { .set = cold_set };
module_param_cb(cold, &cold_ops, NULL, 0200);

/* The key table of another model, from the device tree. Left as it is on any fault. */
static void __init t48s_keypad_dt_keymap(void)
{
	static unsigned short map[NCODES] __initdata;
	struct device_node *np;
	int i, n;
	u32 code, key;

	np = of_find_compatible_node(NULL, NULL, "yealink,t4xs-keypad");
	if (!np)
		return;
	n = of_property_count_u32_elems(np, "yealink,keymap");
	if (n <= 0 || n % 2) {
		if (n != -EINVAL)	/* -EINVAL: no such property, the built-in table stands */
			pr_err(DRV_NAME ": yealink,keymap is not a list of pairs, using the built-in table\n");
		goto out;
	}
	for (i = 0; i < n; i += 2) {
		if (of_property_read_u32_index(np, "yealink,keymap", i, &code) ||
		    of_property_read_u32_index(np, "yealink,keymap", i + 1, &key) ||
		    code == 0 || code >= NCODES || key == KEY_RESERVED || key > KEY_MAX) {
			pr_err(DRV_NAME ": yealink,keymap entry %d is out of range, using the built-in table\n", i / 2);
			goto out;
		}
		map[code] = key;
	}
	memcpy(t48s_keymap, map, sizeof(t48s_keymap));
	pr_info(DRV_NAME ": %d keys from the device tree\n", n / 2);
out:
	of_node_put(np);
}

static int __init t48s_keypad_init(void)
{
	u32 ctrl;
	int i, rc;

	if (!of_machine_is_compatible("yealink,sip-t4xs"))
		return -ENODEV;
	t48s_keypad_dt_keymap();
	keyc = ioremap(KEYC_PHYS, 0x1000);
	gpio0 = ioremap(GPIO0_PHYS, 0x1000);
	rc = -ENOMEM;
	if (!keyc || !gpio0)
		goto err_map;

	ctrl = readl(keyc + KEYC_CTRL);
	if (ctrl & KEYC_CTRL_EN) {
		pr_info(DRV_NAME ": controller found running (control 0x%x, prescaler 0x%x)\n",
			ctrl, readl(keyc + KEYC_PRESCALE));
	} else {
		pr_info(DRV_NAME ": controller is off (control 0x%x), starting it\n", ctrl);
		rc = t48s_keypad_start();
		if (rc)
			goto err_map;
	}

	keys_dev = input_allocate_device();
	hook_dev = input_allocate_device();
	if (!keys_dev || !hook_dev) {
		rc = -ENOMEM;
		goto err_alloc;
	}

	keys_dev->name = "T48S keypad";
	keys_dev->phys = "t48s/keypad";
	keys_dev->id.bustype = BUS_HOST;
	keys_dev->keycode = t48s_keymap;
	keys_dev->keycodesize = sizeof(t48s_keymap[0]);
	keys_dev->keycodemax = NCODES;
	__set_bit(EV_KEY, keys_dev->evbit);
	__set_bit(EV_REP, keys_dev->evbit);
	input_set_capability(keys_dev, EV_MSC, MSC_SCAN);
	for (i = 0; i < NCODES; i++)
		if (t48s_keymap[i] != KEY_RESERVED)
			__set_bit(t48s_keymap[i], keys_dev->keybit);
	__set_bit(KEY_UNKNOWN, keys_dev->keybit);	/* a position not in the table */

	/* its own device: a lifted handset must not auto-repeat */
	hook_dev->name = "T48S hook switch";
	hook_dev->phys = "t48s/hook";
	hook_dev->id.bustype = BUS_HOST;
	input_set_capability(hook_dev, EV_KEY, KEY_PHONE);

	rc = input_register_device(keys_dev);
	if (rc)
		goto err_alloc;
	rc = input_register_device(hook_dev);
	if (rc) {
		input_unregister_device(keys_dev);
		keys_dev = NULL;
		goto err_alloc;
	}

	/* keys already held at boot are reported by the first poll */
	t48s_keypad_read(last_col);
	for (i = 0; i < COLS; i++) {
		if (last_col[i])
			pr_info(DRV_NAME ": column %d reads 0x%03x at start (keys held?)\n",
				i, last_col[i]);
		last_col[i] = 0;
	}
	pr_info(DRV_NAME ": hook GPIO29 = %d at start\n",
		!!(readl(gpio0 + GPIO_DATA) & HOOK_BIT));

	timer_setup(&poll_timer, t48s_keypad_poll, 0);
	mod_timer(&poll_timer, jiffies + POLL_JIFFIES);
	return 0;

err_alloc:
	input_free_device(keys_dev);
	input_free_device(hook_dev);
err_map:
	if (keyc)
		iounmap(keyc);
	if (gpio0)
		iounmap(gpio0);
	return rc;
}
device_initcall(t48s_keypad_init);
MODULE_LICENSE("GPL");
