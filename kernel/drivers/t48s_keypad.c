/*
 * t48s_keypad: the keys and the hook switch of the Yealink SIP-T48S.
 *
 * Keys: the SoC's own keypad controller (KEYC) at 0x8000D000 scans a matrix
 * on GPIO43-58 and keeps the result in five status words, two columns of nine
 * row bits per word, a 0 bit meaning pressed. U-Boot (and Yealink's kernel)
 * leave it running, so normally there is nothing to set up.
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

	[36] = KEY_F10,		/* Speakerphone */

	/* guesses: Yealink F14 and F15 sit next to each other, like a volume bar */
	[45] = KEY_VOLUMEDOWN,	/* Yealink F14 */
	[44] = KEY_VOLUMEUP,	/* Yealink F15 */

	/* labels unknown: headset, mute, message, hold, redial, transfer in some order */
	[25] = KEY_F7,
	[37] = KEY_F8,
	[35] = KEY_F9,
	[34] = KEY_F16,
	[33] = KEY_F17,
	[26] = KEY_F20,
};

static void __iomem *keyc;
static void __iomem *gpio0;
static struct input_dev *keys_dev;
static struct input_dev *hook_dev;
static struct timer_list poll_timer;
static u16 last_col[COLS];	/* 1 bit = pressed */
static int hook_level = -1;	/* last reported GPIO level */
static int hook_seen = -1;	/* level at the previous poll, for debounce */

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

static void t48s_keypad_poll(unsigned long unused)
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
			if (key != KEY_RESERVED)
				input_report_key(keys_dev, key, down);
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
 * running); here for a boot path that skips both. Never exercised so far.
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
	v |= KEYC_CTRL_EN;
	writel(v, keyc + KEYC_CTRL);
	v |= KEYC_CTRL_IRQEN;
	writel(v, keyc + KEYC_CTRL);
	msleep(50);	/* let it scan once before the first read */
	return 0;
}

static int __init t48s_keypad_init(void)
{
	u32 ctrl;
	int i, rc;

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

	setup_timer(&poll_timer, t48s_keypad_poll, 0);
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
