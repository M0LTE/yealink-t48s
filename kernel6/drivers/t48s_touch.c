// SPDX-License-Identifier: GPL-2.0
/*
 * t48s_touch: the resistive touchscreen of the Yealink SIP-T48S.
 *
 * A TI TSC2046 (ADS7846 family) sits on the SoC's SPI host, an ARM PL022 at
 * 0x80015000. Chip select is GPIO25 (active low), driven by hand; the chip's
 * pen-down output is on GPIO26 (low = touched). SPI pins are GPIO22-24.
 *
 * This driver talks to the PL022 directly and polls: every 10 ms it looks at
 * the pen-down pin, and only while that is low does it clock the chip. This
 * kernel has no GPIO interrupts and no SPI core built in, and one transfer of
 * about 50 bytes at 2 MHz takes a quarter of a millisecond, so there is
 * nothing to gain from more machinery. U-Boot and Yealink's kernel leave the
 * SPI host set up (8 bits, mode 0, 2 MHz); it is set up again here anyway.
 *
 * Reported as "T48S touchscreen": BTN_TOUCH, ABS_X, ABS_Y and ABS_PRESSURE,
 * all raw 12-bit values (0-4095), the same as Yealink's "YL tsc" device, so
 * the same tslib-style calibration (/etc/pointercal) applies. Yealink's
 * numbers for this phone, from U-Boot's "calparam":
 *   screen_x = (14798 * x + 13 * y - 4134128) / 65536
 *   screen_y = (53 * x + 10207 * y - 5441552) / 65536
 *
 * One reading: X ten times, Y ten times, the two pressure channels twice
 * each, then a power-down command that turns the pen-down output back on.
 * Per axis the first two samples are dropped (the panel is still settling),
 * the rest sorted and the middle four averaged. A reading is thrown away if
 * those four disagree or the pressure channel says nothing is touching.
 *
 * To look at it by hand:
 *   cat /sys/module/t48s_touch/parameters/diag
 * reads the chip's temperature, battery and aux inputs, the pen pin and the
 * counters, without needing a touch. And
 *   echo 1 > /sys/module/t48s_touch/parameters/debug
 * logs every reading taken while the screen is touched.
 *
 * See docs/keypad-touch-hw.md.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/irqflags.h>
#include <linux/moduleparam.h>
#include <linux/of.h>

#define DRV_NAME	"t48s_touch"

#define CHIPC_PHYS	0x80000000
#define CHIPC_SPICLK	0x24		/* SPI clock divider, left as found */
#define CHIPC_BUSCLK	0x5c		/* bus clock gates */
#define CHIPC_BUSCLK_SPIH	0x00400000
#define CHIPC_GPIOMUX(pin)	(0x118 + 4 * ((pin) / 8))	/* 4 bits per pin */
#define GPIOMUX_SPI	3
#define SPI_FIRST_PIN	22
#define SPI_LAST_PIN	24

#define GPIO0_PHYS	0x80004000	/* GPIO0-31, one bit per pin */
#define GPIO_DATA	0x000
#define GPIO_DIR	0x400		/* 1 = output */
#define CS_BIT		(1u << 25)	/* chip select, low = selected */
#define PEN_BIT		(1u << 26)	/* pen down, low = touched */

/* ARM PL022. The registers are 16 bits wide and read back doubled into the top half. */
#define SSP_PHYS	0x80015000
#define SSP_CR0		0x00
#define SSP_CR1		0x04
#define SSP_DR		0x08
#define SSP_SR		0x0c
#define SSP_CPSR	0x10
#define SSP_IMSC	0x14
#define SSP_ICR		0x20
#define SSP_PERIPHID(i)	(0xfe0 + 4 * (i))
#define SSP_CR0_8BIT	0x0007		/* 8 bits, Motorola SPI, mode 0, no extra divide */
#define SSP_CR1_SSE	0x0002		/* enable, master */
#define SSP_SR_TNF	0x02		/* transmit FIFO not full */
#define SSP_SR_RNE	0x04		/* receive FIFO not empty */
#define SSP_SR_BSY	0x10
#define SSP_CPSR_2MHZ	50		/* 100 MHz SPI clock / 50 */
#define SSP_FIFO	8
#define SSP_ID		0x00041022	/* PrimeCell id of a PL022 ... */
#define SSP_ID_MASK	0x000fffff	/* ... ignoring the revision */

/*
 * TSC2046 control byte: start, channel, 12 bits, then the reference and power
 * bits. xxx1 = differential, chip stays awake, pen-down output off (what
 * Yealink uses for positions). x111 = single ended with the internal 2.5 V
 * reference on. Power bits 00 = power down and turn the pen-down output on.
 */
#define TSC_X		0xd1
#define TSC_Y		0x91
#define TSC_Z1		0xb1
#define TSC_Z2		0xc1
#define TSC_PWRDOWN	0x90
#define TSC_TEMP0	0x87
#define TSC_TEMP1	0xf7
#define TSC_VBAT	0xa7
#define TSC_AUX		0xe7

#define TSC_MAX		4095
#define NSAMPLES	10		/* conversions per axis in one reading */
#define NSKIP		2		/* first ones dropped: panel settling */
#define NZ		2		/* conversions per pressure channel */
#define MAX_SPREAD	100		/* the middle four samples must agree this well */
#define MIN_Z1		16		/* below this nothing is pressing on the panel */

#define POLL_JIFFIES	msecs_to_jiffies(10)

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "log every reading taken while the screen is touched");

static void __iomem *ssp;
static void __iomem *gpio0;
static struct input_dev *touch_dev;
static struct delayed_work poll_work;
static DEFINE_MUTEX(spi_lock);
static bool touching;		/* BTN_TOUCH 1 has been reported */
static int up_polls;		/* polls in a row with the pen pin high */
static unsigned int n_readings, n_rejected, n_touches, n_spi_errors;

static u32 ssp_read(int reg) { return readl(ssp + reg) & 0xffff; }
static void ssp_write(u32 v, int reg) { writel(v, ssp + reg); }

/* the GPIO registers have no set/clear form: read, change, write, interrupts off */
static void gpio_change(int reg, u32 bit, int on)
{
	unsigned long flags;
	u32 v;

	local_irq_save(flags);
	v = readl(gpio0 + reg);
	writel(on ? v | bit : v & ~bit, gpio0 + reg);
	local_irq_restore(flags);
}

static int pen_is_down(void)
{
	return !(readl(gpio0 + GPIO_DATA) & PEN_BIT);
}

static void ssp_setup(void)
{
	ssp_write(0, SSP_CR1);
	ssp_write(0, SSP_IMSC);		/* polled: no interrupts */
	ssp_write(3, SSP_ICR);
	ssp_write(SSP_CR0_8BIT, SSP_CR0);
	ssp_write(SSP_CPSR_2MHZ, SSP_CPSR);
	ssp_write(SSP_CR1_SSE, SSP_CR1);
}

/* One full-duplex transfer with chip select held low. Call with spi_lock held. */
static int spi_xfer(const u8 *tx, u8 *rx, int len)
{
	unsigned long end = jiffies + msecs_to_jiffies(100);
	int sent = 0, got = 0, rc = 0;
	u32 sr;

	while (ssp_read(SSP_SR) & SSP_SR_RNE)
		ssp_read(SSP_DR);

	gpio_change(GPIO_DATA, CS_BIT, 0);
	while (got < len) {
		sr = ssp_read(SSP_SR);
		if (sent < len && sent - got < SSP_FIFO && (sr & SSP_SR_TNF)) {
			ssp_write(tx[sent++], SSP_DR);
			continue;
		}
		if (sr & SSP_SR_RNE) {
			rx[got++] = ssp_read(SSP_DR);
			continue;
		}
		if (time_after(jiffies, end)) {
			rc = -ETIMEDOUT;
			break;
		}
		cpu_relax();
	}
	while (!rc && (ssp_read(SSP_SR) & SSP_SR_BSY)) {
		if (time_after(jiffies, end))
			rc = -ETIMEDOUT;
		cpu_relax();
	}
	gpio_change(GPIO_DATA, CS_BIT, 1);

	if (rc) {
		n_spi_errors++;
		ssp_setup();	/* empties the FIFOs */
	}
	return rc;
}

/* the 12-bit answer to the command sent in byte i arrives in bytes i+1 and i+2 */
static int tsc_value(const u8 *rx, int i)
{
	return ((rx[i + 1] & 0x7f) << 5) | (rx[i + 2] >> 3);
}

/* drop the first NSKIP samples, sort the rest, average the middle four */
static int tsc_filter(int *v, int *spread)
{
	int n = NSAMPLES - NSKIP, i, j, t, lo;

	v += NSKIP;
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && v[j - 1] > v[j]; j--) {
			t = v[j];
			v[j] = v[j - 1];
			v[j - 1] = t;
		}
	lo = (n - 4) / 2;
	*spread = v[lo + 3] - v[lo];
	return (v[lo] + v[lo + 1] + v[lo + 2] + v[lo + 3] + 2) / 4;
}

struct tsc_reading {
	int x, y, z1, z2;
	int xspread, yspread;
};

#define READING_LEN	(2 * (2 * NSAMPLES + 2 * NZ) + 3)

static int tsc_read_position(struct tsc_reading *r)
{
	u8 tx[READING_LEN], rx[READING_LEN];
	int xs[NSAMPLES], ys[NSAMPLES];
	int i, n = 0, rc;

	memset(tx, 0, sizeof(tx));
	for (i = 0; i < NSAMPLES; i++, n += 2)
		tx[n] = TSC_X;
	for (i = 0; i < NSAMPLES; i++, n += 2)
		tx[n] = TSC_Y;
	for (i = 0; i < NZ; i++, n += 2)
		tx[n] = TSC_Z1;
	for (i = 0; i < NZ; i++, n += 2)
		tx[n] = TSC_Z2;
	tx[n] = TSC_PWRDOWN;

	mutex_lock(&spi_lock);
	rc = spi_xfer(tx, rx, sizeof(tx));
	mutex_unlock(&spi_lock);
	if (rc)
		return rc;

	for (i = 0; i < NSAMPLES; i++) {
		xs[i] = tsc_value(rx, 2 * i);
		ys[i] = tsc_value(rx, 2 * (NSAMPLES + i));
	}
	/* the second conversion of each pressure channel, the first is settling */
	r->z1 = tsc_value(rx, 2 * (2 * NSAMPLES + NZ - 1));
	r->z2 = tsc_value(rx, 2 * (2 * NSAMPLES + 2 * NZ - 1));
	r->x = tsc_filter(xs, &r->xspread);
	r->y = tsc_filter(ys, &r->yspread);
	return 0;
}

static void t48s_touch_poll(struct work_struct *work)
{
	struct tsc_reading r;
	int pressure;

	if (!pen_is_down()) {
		/* up at two polls in a row: released */
		if (touching && ++up_polls >= 2) {
			touching = false;
			if (debug)
				pr_info(DRV_NAME ": up\n");
			input_report_key(touch_dev, BTN_TOUCH, 0);
			input_report_abs(touch_dev, ABS_PRESSURE, 0);
			input_sync(touch_dev);
		}
		goto again;
	}
	up_polls = 0;

	if (tsc_read_position(&r))
		goto again;
	n_readings++;
	if (r.z1 < MIN_Z1 || r.xspread > MAX_SPREAD || r.yspread > MAX_SPREAD) {
		n_rejected++;
		if (debug)
			pr_info(DRV_NAME ": rejected x %d (spread %d) y %d (spread %d) z1 %d z2 %d\n",
				r.x, r.xspread, r.y, r.yspread, r.z1, r.z2);
		goto again;
	}

	/* harder press = lower resistance between the layers = z1 and z2 closer together */
	pressure = clamp(TSC_MAX + r.z1 - r.z2, 1, TSC_MAX);
	if (debug)
		pr_info(DRV_NAME ": %s x %d (spread %d) y %d (spread %d) z1 %d z2 %d pressure %d\n",
			touching ? "move" : "down", r.x, r.xspread, r.y, r.yspread,
			r.z1, r.z2, pressure);
	if (!touching) {
		touching = true;
		n_touches++;
		input_report_key(touch_dev, BTN_TOUCH, 1);
	}
	input_report_abs(touch_dev, ABS_X, r.x);
	input_report_abs(touch_dev, ABS_Y, r.y);
	input_report_abs(touch_dev, ABS_PRESSURE, pressure);
	input_sync(touch_dev);

again:
	schedule_delayed_work(&poll_work, POLL_JIFFIES);
}

/*
 * The chip's other inputs, read with its internal 2.5 V reference: a way to
 * see it answer without a touch. The two temperature readings are a diode
 * voltage at two currents; their difference gives degrees.
 */
static int tsc_read_aux(int *temp0, int *temp1, int *vbat, int *aux)
{
	static const u8 wake[3] = { TSC_TEMP0, 0, 0 };
	static const u8 tx[11] = { TSC_TEMP0, 0, TSC_TEMP1, 0, TSC_VBAT, 0,
				   TSC_AUX, 0, TSC_PWRDOWN, 0, 0 };
	u8 rx[11];
	int rc;

	mutex_lock(&spi_lock);
	rc = spi_xfer(wake, rx, sizeof(wake));	/* reference on ... */
	if (!rc) {
		msleep(10);			/* ... and settled */
		rc = spi_xfer(tx, rx, sizeof(tx));
	}
	mutex_unlock(&spi_lock);
	if (rc)
		return rc;
	*temp0 = tsc_value(rx, 0);
	*temp1 = tsc_value(rx, 2);
	*vbat = tsc_value(rx, 4);
	*aux = tsc_value(rx, 6);
	return 0;
}

static int diag_get(char *buf, const struct kernel_param *kp)
{
	int temp0, temp1, vbat, aux, rc;

	rc = tsc_read_aux(&temp0, &temp1, &vbat, &aux);
	if (rc)
		return sprintf(buf, "SPI transfer failed (%d)\n", rc);
	/* degrees C = 2.573 per mV of difference - 273, one count = 2500/4096 mV */
	return sprintf(buf,
		"pen %s, temp0 %d temp1 %d (%d C), vbat %d, aux %d, readings %u rejected %u touches %u spi errors %u\n",
		pen_is_down() ? "down" : "up", temp0, temp1,
		(temp1 - temp0) * 15704 / 10000 - 273, vbat, aux,
		n_readings, n_rejected, n_touches, n_spi_errors);
}

/*
 * Test hook: echo "X Y" > /sys/module/t48s_touch/parameters/inject reports a
 * touch at raw position X Y (0 to 4095, as the chip gives them), "up" lifts
 * it. For testing programs without a finger; a real touch still works and
 * simply takes over.
 */
static int inject_set(const char *val, const struct kernel_param *kp)
{
	unsigned int x, y;

	if (!touch_dev)
		return -ENODEV;
	if (!strncmp(val, "up", 2)) {
		input_report_key(touch_dev, BTN_TOUCH, 0);
		input_report_abs(touch_dev, ABS_PRESSURE, 0);
	} else if (sscanf(val, "%u %u", &x, &y) == 2 && x <= TSC_MAX && y <= TSC_MAX) {
		input_report_key(touch_dev, BTN_TOUCH, 1);
		input_report_abs(touch_dev, ABS_X, x);
		input_report_abs(touch_dev, ABS_Y, y);
		input_report_abs(touch_dev, ABS_PRESSURE, TSC_MAX / 2);
	} else {
		return -EINVAL;
	}
	input_sync(touch_dev);
	return 0;
}

static const struct kernel_param_ops inject_ops = { .set = inject_set };
module_param_cb(inject, &inject_ops, NULL, 0200);
MODULE_PARM_DESC(inject, "\"X Y\" (raw, 0 to 4095): report a touch there; \"up\": lift it. For tests");

static const struct kernel_param_ops diag_ops = { .get = diag_get };
module_param_cb(diag, &diag_ops, NULL, 0444);
MODULE_PARM_DESC(diag, "read the chip's temperature, battery and aux inputs, the pen pin and the counters");

/* pins, clock gate and chip select; all of this is normally found done already */
static int t48s_touch_pins(void)
{
	void __iomem *chipc;
	unsigned long flags;
	u32 v, mux[2], busclk;
	int pin;

	chipc = ioremap(CHIPC_PHYS, 0x1000);
	if (!chipc)
		return -ENOMEM;
	mux[0] = readl(chipc + CHIPC_GPIOMUX(SPI_FIRST_PIN));
	mux[1] = readl(chipc + CHIPC_GPIOMUX(SPI_LAST_PIN));
	busclk = readl(chipc + CHIPC_BUSCLK);
	pr_info(DRV_NAME ": found pin mux 0x%08x 0x%08x, bus clocks 0x%08x, SPI clock 0x%08x\n",
		mux[0], mux[1], busclk, readl(chipc + CHIPC_SPICLK));

	local_irq_save(flags);
	for (pin = SPI_FIRST_PIN; pin <= SPI_LAST_PIN; pin++) {
		v = readl(chipc + CHIPC_GPIOMUX(pin));
		v &= ~(0xfu << ((pin % 8) * 4));
		v |= GPIOMUX_SPI << ((pin % 8) * 4);
		writel(v, chipc + CHIPC_GPIOMUX(pin));
	}
	writel(readl(chipc + CHIPC_BUSCLK) | CHIPC_BUSCLK_SPIH, chipc + CHIPC_BUSCLK);
	local_irq_restore(flags);
	iounmap(chipc);

	gpio_change(GPIO_DATA, CS_BIT, 1);
	gpio_change(GPIO_DIR, CS_BIT, 1);
	gpio_change(GPIO_DIR, PEN_BIT, 0);
	return 0;
}

static int __init t48s_touch_init(void)
{
	static const u8 pwrdown[3] = { TSC_PWRDOWN, 0, 0 };
	int temp0, temp1, vbat, aux, i, rc;
	u8 rx[3];
	u32 id = 0;

	if (!of_machine_is_compatible("yealink,sip-t48s"))
		return -ENODEV;
	ssp = ioremap(SSP_PHYS, 0x1000);
	gpio0 = ioremap(GPIO0_PHYS, 0x1000);
	rc = -ENOMEM;
	if (!ssp || !gpio0)
		goto err_map;

	rc = t48s_touch_pins();
	if (rc)
		goto err_map;

	for (i = 0; i < 4; i++)
		id |= (ssp_read(SSP_PERIPHID(i)) & 0xff) << (8 * i);
	if ((id & SSP_ID_MASK) != SSP_ID) {
		pr_err(DRV_NAME ": no PL022 at 0x%08x (id 0x%08x), giving up\n", SSP_PHYS, id);
		rc = -ENODEV;
		goto err_map;
	}
	pr_info(DRV_NAME ": PL022 id 0x%08x, found CR0 0x%04x CR1 0x%04x CPSR %u\n",
		id, ssp_read(SSP_CR0), ssp_read(SSP_CR1), ssp_read(SSP_CPSR));
	ssp_setup();

	/* as Yealink does at start: one power-down command, pen-down output on */
	mutex_lock(&spi_lock);
	rc = spi_xfer(pwrdown, rx, sizeof(pwrdown));
	mutex_unlock(&spi_lock);
	if (!rc)
		rc = tsc_read_aux(&temp0, &temp1, &vbat, &aux);
	if (rc) {
		pr_err(DRV_NAME ": SPI transfer failed (%d), giving up\n", rc);
		goto err_map;
	}
	pr_info(DRV_NAME ": TSC2046 temp0 %d temp1 %d (%d C), vbat %d, aux %d, pen %s\n",
		temp0, temp1, (temp1 - temp0) * 15704 / 10000 - 273, vbat, aux,
		pen_is_down() ? "down" : "up");

	touch_dev = input_allocate_device();
	if (!touch_dev) {
		rc = -ENOMEM;
		goto err_map;
	}
	touch_dev->name = "T48S touchscreen";
	touch_dev->phys = "t48s/touch";
	touch_dev->id.bustype = BUS_SPI;
	input_set_capability(touch_dev, EV_KEY, BTN_TOUCH);
	input_set_abs_params(touch_dev, ABS_X, 0, TSC_MAX, 0, 0);
	input_set_abs_params(touch_dev, ABS_Y, 0, TSC_MAX, 0, 0);
	input_set_abs_params(touch_dev, ABS_PRESSURE, 0, TSC_MAX, 0, 0);
	rc = input_register_device(touch_dev);
	if (rc) {
		input_free_device(touch_dev);
		goto err_map;
	}

	INIT_DELAYED_WORK(&poll_work, t48s_touch_poll);
	schedule_delayed_work(&poll_work, POLL_JIFFIES);
	return 0;

err_map:
	if (ssp)
		iounmap(ssp);
	if (gpio0)
		iounmap(gpio0);
	return rc;
}
device_initcall(t48s_touch_init);
MODULE_LICENSE("GPL");
