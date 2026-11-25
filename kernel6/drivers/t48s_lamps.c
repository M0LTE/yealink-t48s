// SPDX-License-Identifier: GPL-2.0
/*
 * t48s_lamps: the lamps inside the Headset, Mute and Speaker keys of the
 * Yealink SIP-T48S, with brightness.
 *
 * Each lamp is a plain GPIO pin and nothing else: the SoC's one PWM is a
 * single channel on GPIO35, the screen's backlight, and its LED matrix block
 * scans rows and columns and has no level per lamp. So the pins are switched
 * in software, from one high-resolution timer for all of them.
 *
 * Every frame (5 ms, 200 a second) the lamps that are lit go on together and
 * each goes off when its share of the frame is over. A lamp that is fully on
 * or off costs nothing, and with none in between the timer does not run. The
 * handler waits out anything shorter than 50 us with interrupts off instead of
 * taking another interrupt, so the short pulses of a dim lamp are exact. A
 * longer one ends on a timer interrupt asked for 12 us early, which is about
 * as late as an interrupt usually comes here; the rest is waited out, so that
 * pulse is exact too unless the interrupt was later than that. A pulse is
 * measured from the moment the lamp went on, not from when the frame should
 * have started.
 *
 * They are ordinary LED devices, /sys/class/leds/<label>/brightness, 0 to 255.
 * Brightness is as the eye takes it, not time lit: the time is the CIE
 * lightness curve of the number (1 is 2 us of the frame, 128 is 22%), so equal
 * steps look equal and a fade is a plain count. The timer trigger and the
 * other LED triggers work as on any lamp.
 *
 * Device tree: compatible "yealink,t4xs-lamps", one child for each lamp with
 * "label" and "gpios", as for gpio-leds.
 *
 * What it costs, counted since the last write of 0 to "frames":
 *   /sys/module/t48s_lamps/parameters/
 *     frame_us      the frame, 2000 to 20000
 *     frames        frames run
 *     irqs          timer interrupts taken
 *     busy_ns       time spent in them, the waits included
 *     late_us       the latest a frame started
 *     off_late_us   the latest a lamp went off after its time (0: never late)
 *     late_sum_us   all the interrupts' lateness added up
 *
 * GPL v2.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/leds.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/spinlock.h>

#define DRV_NAME	"t48s_lamps"
#define MAX_LAMPS	4
#define BUSY_NS		50000		/* wait this long or less in the handler; more, take another interrupt */
#define LEAD_NS		12000		/* ask for a lamp's off interrupt this early, and wait the rest out */
#define FULL_NS		4000		/* dark for less than this in a frame: leave the lamp on */

struct lamp {
	struct led_classdev cdev;
	struct gpio_desc *gpio;
	u8 level;			/* asked for, 0 to 255 */
};

static struct lamp lamps[MAX_LAMPS];
static int nlamps;
static DEFINE_RAW_SPINLOCK(lamps_lock);
static struct hrtimer lamps_timer;
static bool running;			/* the timer is armed, or its handler is at work */
static u32 on_ns[256];			/* time lit in a frame, by level */

/* this frame: the lamps that go off inside it, soonest first */
static struct { u32 ns; struct gpio_desc *gpio; } plan[MAX_LAMPS];
static int nplan, phase;
static ktime_t lit_at;			/* when this frame's lamps went on */
static ktime_t next_frame;

static unsigned int frame_us = 5000;
static unsigned int frames, irqs, late_us, off_late_us, late_sum_us;
static unsigned long long busy_ns;
module_param(frames, uint, 0644);
module_param(irqs, uint, 0644);
module_param(busy_ns, ullong, 0644);
module_param(late_us, uint, 0644);
module_param(off_late_us, uint, 0644);
module_param(late_sum_us, uint, 0644);

/*
 * The CIE 1931 lightness curve, in whole numbers: the share of the frame
 * (of 65536) that looks like level b of 255.
 */
static u32 share_of(unsigned int b)
{
	u32 l = b * 100000 / 255;	/* lightness, thousandths, 0 to 100000 */
	u32 r;

	if (b >= 255)
		return 65536;
	if (l <= 8000)
		return l * 65536 / 903300;
	r = (l + 16000) * 4096 / 7250;	/* (L + 16) / 116, of 65536 */
	return (u32)(((u64)r * r >> 16) * r >> 16);
}

static void lamps_curve(void)
{
	unsigned int b;

	for (b = 0; b < 256; b++)
		on_ns[b] = (u32)((u64)share_of(b) * frame_us * 1000 >> 16);
}

/* Start a frame: every lamp to where it starts, and the list of those that go off in it. Lock held. */
static void frame_begin(void)
{
	u32 frame_ns = frame_us * 1000;
	int i, j;

	nplan = 0;
	for (i = 0; i < nlamps; i++) {
		u32 ns = on_ns[lamps[i].level];

		gpiod_set_value(lamps[i].gpio, ns != 0);
		if (!ns || ns + FULL_NS >= frame_ns)
			continue;
		for (j = nplan++; j > 0 && plan[j - 1].ns > ns; j--)
			plan[j] = plan[j - 1];
		plan[j].ns = ns;
		plan[j].gpio = lamps[i].gpio;
	}
	lit_at = ktime_get();
	phase = 0;
	frames++;
}

static enum hrtimer_restart lamps_tick(struct hrtimer *t)
{
	ktime_t in = ktime_get(), now = in, due;
	bool first = true, off;
	s64 d;

	raw_spin_lock(&lamps_lock);
	irqs++;
	for (;;) {
		off = phase < nplan;
		due = off ? ktime_add_ns(lit_at, plan[phase].ns) : next_frame;
		d = ktime_to_ns(ktime_sub(due, now));
		if (d > BUSY_NS)
			break;
		if (d > 0) {
			udelay((u32)d / 1000);
		} else if (first) {
			/* what the interrupt was for, and how late it came */
			u32 late = (u32)(-d) / 1000;

			late_sum_us += late;
			if (off && late > off_late_us)
				off_late_us = late;
			if (!off && late > late_us)
				late_us = late;
		}
		first = false;
		if (off) {
			gpiod_set_value(plan[phase++].gpio, 0);
		} else {
			frame_begin();
			/* keep to the frame's own beat unless we have fallen a whole frame behind */
			next_frame = ktime_add_us(next_frame, frame_us);
			if (ktime_before(next_frame, lit_at))
				next_frame = ktime_add_us(lit_at, frame_us);
			if (!nplan) {
				/* nothing between on and off any more: no timer */
				running = false;
				break;
			}
		}
		now = ktime_get();
	}
	if (running)
		hrtimer_set_expires(t, off ? ktime_sub_ns(due, LEAD_NS) : due);
	busy_ns += ktime_to_ns(ktime_sub(ktime_get(), in));
	raw_spin_unlock(&lamps_lock);
	return running ? HRTIMER_RESTART : HRTIMER_NORESTART;
}

static void lamp_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct lamp *l = container_of(cdev, struct lamp, cdev);
	u32 frame_ns = frame_us * 1000, ns;
	unsigned long flags;

	raw_spin_lock_irqsave(&lamps_lock, flags);
	l->level = min_t(unsigned int, value, 255);
	ns = on_ns[l->level];
	if (running) {
		/* the next frame takes it up */
	} else if (!ns || ns + FULL_NS >= frame_ns) {
		gpiod_set_value(l->gpio, ns != 0);
	} else {
		/* a frame starts now, in the timer's handler */
		running = true;
		nplan = phase = 0;
		next_frame = ktime_get();
		hrtimer_start(&lamps_timer, next_frame, HRTIMER_MODE_ABS);
	}
	raw_spin_unlock_irqrestore(&lamps_lock, flags);
}

static int frame_us_set(const char *val, const struct kernel_param *kp)
{
	unsigned long flags;
	unsigned int us;
	int rc;

	rc = kstrtouint(val, 0, &us);
	if (rc)
		return rc;
	if (us < 2000 || us > 20000)
		return -EINVAL;
	raw_spin_lock_irqsave(&lamps_lock, flags);
	frame_us = us;
	lamps_curve();
	raw_spin_unlock_irqrestore(&lamps_lock, flags);
	return 0;
}

static const struct kernel_param_ops frame_us_ops = {
	.set = frame_us_set,
	.get = param_get_uint,
};
module_param_cb(frame_us, &frame_us_ops, &frame_us, 0644);
MODULE_PARM_DESC(frame_us, "one frame of the lamps' brightness, in microseconds");

/* probe failed part of the way: nothing of ours may still be switching pins */
static int lamps_abandon(struct fwnode_handle *child, int rc)
{
	unsigned long flags;

	fwnode_handle_put(child);
	raw_spin_lock_irqsave(&lamps_lock, flags);
	nlamps = nplan = phase = 0;
	raw_spin_unlock_irqrestore(&lamps_lock, flags);
	hrtimer_cancel(&lamps_timer);
	running = false;
	return rc;
}

static int t48s_lamps_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *child;
	int rc;

	lamps_curve();
	hrtimer_setup(&lamps_timer, lamps_tick, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	device_for_each_child_node(dev, child) {
		struct led_init_data init = { .fwnode = child };
		struct lamp *l = &lamps[nlamps];

		if (nlamps == MAX_LAMPS) {
			dev_warn(dev, "more than %d lamps: the rest are left out\n", MAX_LAMPS);
			fwnode_handle_put(child);
			break;
		}
		l->gpio = devm_fwnode_gpiod_get(dev, child, NULL, GPIOD_OUT_LOW, NULL);
		if (IS_ERR(l->gpio))
			return lamps_abandon(child, dev_err_probe(dev, PTR_ERR(l->gpio), "no pin for a lamp\n"));
		if (gpiod_cansleep(l->gpio))
			return lamps_abandon(child, dev_err_probe(dev, -EINVAL, "a lamp's pin cannot be switched from an interrupt\n"));
		l->cdev.max_brightness = 255;
		l->cdev.brightness_set = lamp_set;
		/* counted before it is registered: a trigger may set it at once */
		nlamps++;
		rc = devm_led_classdev_register_ext(dev, &l->cdev, &init);
		if (rc)
			return lamps_abandon(child, dev_err_probe(dev, rc, "lamp not registered\n"));
	}
	dev_info(dev, "%d lamps, 255 levels, %u frames a second\n", nlamps, 1000000 / frame_us);
	return 0;
}

static const struct of_device_id t48s_lamps_of_match[] = {
	{ .compatible = "yealink,t4xs-lamps" },
	{ }
};

static struct platform_driver t48s_lamps_driver = {
	.probe = t48s_lamps_probe,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = t48s_lamps_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(t48s_lamps_driver);
MODULE_LICENSE("GPL");
