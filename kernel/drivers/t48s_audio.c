// SPDX-License-Identifier: GPL-2.0
/*
 * t48s_audio: microphone capture for the Yealink SIP-T48S (Broadcom BCM11188).
 *
 * The SoC's APM block (0x30600000) holds two audio codecs, "A" and "B", each
 * one ADC and one DAC running at 16 kHz. Each ADC has a two-way analog input
 * switch (inputs "A" and "B") and a preamp, and fills a small FIFO that is a
 * request source on DMA controller 0 (a Synopsys DW AHB DMAC at 0x30200000):
 * channel 3 for codec A, channel 5 for codec B. No DSP and no vendor module
 * is involved (the voice DSP is halted here if it is still running).
 *
 * ALSA view: one card, "T48S", with two capture devices:
 *   hw:0,0  codec A      hw:0,1  codec B
 * both S16_LE, mono, 16000 Hz. Periods are whole 5 ms blocks (80 frames).
 *
 * Mixer controls, per codec ("Codec A ...", "Codec B ..."):
 *   Capture Switch          ADC analog front end power (Broadcom uses this as
 *                           the microphone mute)
 *   Capture Source          analog input A or B
 *   Preamp Capture Volume   0 to 42 dB in 3 dB steps
 *   Capture Volume          ADC digital gain, mute or -35 to +12 dB
 *   Mic Bias A Switch       bias supply of input A (tied to mic detection)
 *   Mic Bias B Switch       bias supply of input B
 *
 * CAPTURE ONLY. This driver never writes a DAC FIFO, a DAC gain, an output
 * mode, mute or amplifier bit, sidetone, or GPIO31 (the loudspeaker amplifier).
 * But note: Yealink's firmware idles with codec B's output B switched on and
 * codec B's sidetone enabled (microphone fed to that output). After a kexec
 * boot that is still so, which means codec B's capture controls can change
 * what reaches that output. Codec A has no such path at idle. Until the
 * playback side exists and switches sidetone itself, treat codec B's controls
 * as audible. See docs/audio.md.
 *
 * It also does not initialise the APM block: clocks, sample rate, FIFO and
 * codec enables are used as found (true after a kexec from Yealink's kernel).
 * A cold start needs the reset and init sequence in docs/audio.md, which
 * touches the output side and is not here yet.
 *
 * DMA: the channel runs a ring of linked-list items, one per 5 ms block,
 * straight into the ALSA buffer, with the control words Yealink's kernel was
 * seen using (CTL 0x1a204827, CFG 0xc0 / 0x106 or 0x206). The item that ends
 * a period raises the block interrupt. The ADC never stops, so while nothing
 * records its FIFO sits full of old samples (64 of them, measured); each
 * start first reads one block into a scrap buffer to get rid of those. Our kernel's DMA framework is patched
 * to leave controller 0 alone (kernel/build.sh); this driver programs only
 * its own channels' registers and the per-channel bits of the shared ones,
 * so the other six channels are never touched.
 *
 * The register map comes from the ApmHw_* functions in Yealink's kernel image
 * and from Broadcom's GPL halaudio APM driver, which calls them.
 *
 * Counters and registers: cat /proc/asound/card0/t48s
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/spinlock.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/info.h>
#include <mach/irqs.h>

#define DRV_NAME	"t48s_audio"

#define APM_PHYS	0x30600000
#define APM_SIZE	0x1000
#define DMA0_PHYS	0x30200000
#define DMA0_SIZE	0x1000
#define CHIPC_PHYS	0x80000000
#define CHIPC_SIZE	0x4000

/* chip control: the voice DSP (VPM) */
#define CHIPC_CLKEN		0x10
#define CHIPC_CLKEN_VPM		0x00010000
#define CHIPC_VPM_HALT		0x301c

/* APM */
#define APM_IRQSTAT		0x004	/* write 1 to clear */
#define APM_IRQEN		0x008
#define APM_IRQ_ADC_OVF_A	0x40
#define APM_IRQ_ADC_OVF_B	0x80
#define APM_CTL			0x00c	/* top four bits: codec enables */
#define APM_ADC_DGAIN_A		0x040	/* bits 24-27 shift, low 16 bits gain */
#define APM_RATE_A		0x044
#define APM_ADC_DGAIN_B		0x070
#define APM_RATE_B		0x074
#define APM_RATE_16K		0x40000000
#define APM_ANA			0x120	/* ADC power and input B mic bias */
#define APM_ANA_ADC_A		0x0000000c	/* 2 = on, 0 = off */
#define APM_ANA_ADC_A_ON	0x00000008
#define APM_ANA_ADC_B		0x00000060
#define APM_ANA_ADC_B_ON	0x00000040
#define APM_ANA_BIASB_OFF_A	0x00000002
#define APM_ANA_BIASB_OFF_B	0x00000010
#define APM_AFE_A		0x124	/* ADC analog front end */
#define APM_AFE_B		0x12c
#define APM_AFE_INPUT_B		0x02000000
#define APM_AFE_PGA_SHIFT	19		/* preamp gain in dB, 0 to 42 */
#define APM_AFE_PGA_MASK	0x01f80000
#define APM_MICDET_A		0x42c	/* bit 1: input A mic bias off */
#define APM_MICDET_B		0x52c
#define APM_MICDET_BIASA_OFF	0x00000002

#define DGAIN_UNITY		0x29f1	/* 0 dB at 16 kHz (Broadcom's scale) */
#define DGAIN_KEEP		0xf0ff0000
#define DGAIN_MAX_STEP		48	/* 0 = mute, n = n - 36 dB */
#define DGAIN_0DB_STEP		36
#define PGA_MAX_STEP		14	/* 3 dB each */

/* DW AHB DMAC */
#define DMAC_CH(c)		((c) * 0x58)
#define DMAC_SAR		0x00
#define DMAC_DAR		0x08
#define DMAC_LLP		0x10
#define DMAC_CTL_LO		0x18
#define DMAC_CTL_HI		0x1c
#define DMAC_CFG_LO		0x40
#define DMAC_CFG_HI		0x44
#define DMAC_STATUS_BLOCK	0x2f0
#define DMAC_STATUS_ERR		0x308
#define DMAC_MASK_TFR		0x310
#define DMAC_MASK_BLOCK		0x318
#define DMAC_MASK_SRCTRAN	0x320
#define DMAC_MASK_DSTTRAN	0x328
#define DMAC_MASK_ERR		0x330
#define DMAC_CLEAR_TFR		0x338
#define DMAC_CLEAR_BLOCK	0x340
#define DMAC_CLEAR_SRCTRAN	0x348
#define DMAC_CLEAR_DSTTRAN	0x350
#define DMAC_CLEAR_ERR		0x358
#define DMAC_CFG		0x398
#define DMAC_CHEN		0x3a0

/*
 * Seen on Yealink's running ADC channels: both list pointers enabled, device
 * to memory with the DMAC counting, 32-bit reads from the FIFO in bursts of
 * four. Bit 0 (interrupt enable) is added on the item that ends a period.
 */
#define RX_CTL_LO		0x1a204826
#define CTL_INT_EN		0x00000001
#define CTL_LLP_EN		0x18000000
#define RX_CFG_LO		0x000000c0

#define BLK_WORDS		40	/* 32-bit FIFO words per block */
#define BLK_BYTES		160	/* 80 samples, 5 ms */
#define MAX_BLKS		512	/* 2.56 s */
#define PRE_ITEM		MAX_BLKS	/* list item that drains the FIFO first */
#define LLI_BYTES		((MAX_BLKS + 1) * sizeof(struct lli) + BLK_BYTES)
#define SCRAP_OFF		((MAX_BLKS + 1) * sizeof(struct lli))
#define OVF_LIMIT		100	/* overflow interrupts per run before giving up */

struct lli {			/* DMAC linked list item, as the hardware reads it */
	u32 sar;
	u32 dar;
	u32 llp;
	u32 ctl_lo;
	u32 ctl_hi;
	u32 sstat;
	u32 dstat;
	u32 pad;		/* keep each item 8-byte aligned */
};

struct t48s_codec_desc {
	const char *name;
	int dma_ch;
	int irq;
	u32 fifo;		/* ADC FIFO, physical */
	u32 cfg_hi;		/* FIFO mode, source request line */
	u32 ovf_bit;		/* in APM_IRQSTAT / APM_IRQEN */
	u32 dgain_reg, rate_reg, afe_reg, micdet_reg;
	u32 adc_mask, adc_on, biasb_off;
};

static const struct t48s_codec_desc t48s_codecs[2] = {
	{ "codec A", 3, IRQ_DMA0C3, 0x30670000, 0x106, APM_IRQ_ADC_OVF_A,
	  APM_ADC_DGAIN_A, APM_RATE_A, APM_AFE_A, APM_MICDET_A,
	  APM_ANA_ADC_A, APM_ANA_ADC_A_ON, APM_ANA_BIASB_OFF_A },
	{ "codec B", 5, IRQ_DMA0C5, 0x30690000, 0x206, APM_IRQ_ADC_OVF_B,
	  APM_ADC_DGAIN_B, APM_RATE_B, APM_AFE_B, APM_MICDET_B,
	  APM_ANA_ADC_B, APM_ANA_ADC_B_ON, APM_ANA_BIASB_OFF_B },
};

struct t48s_audio;

struct t48s_codec {
	const struct t48s_codec_desc *d;
	struct t48s_audio *au;
	struct snd_pcm_substream *ss;
	struct lli *lli;
	dma_addr_t lli_dma;
	unsigned int nblk;
	int running;
	int ovf_armed;		/* overflow interrupt enabled for this run */
	unsigned int ovf_run;	/* overflows in this run */
	int dgain_step;

	/* counters, for /proc/asound/card0/t48s */
	unsigned long irqs, overflows, dma_errors, starts;
};

struct t48s_audio {
	struct snd_card *card;
	struct device *dev;
	void __iomem *apm;
	void __iomem *dmac;
	spinlock_t lock;	/* registers shared between the codecs */
	struct t48s_codec codec[2];
};

static inline u32 apm_rd(struct t48s_audio *au, u32 off)
{
	return readl(au->apm + off);
}

static inline void apm_wr(struct t48s_audio *au, u32 off, u32 val)
{
	writel(val, au->apm + off);
}

static void apm_update(struct t48s_audio *au, u32 off, u32 mask, u32 val)
{
	unsigned long flags;

	spin_lock_irqsave(&au->lock, flags);
	apm_wr(au, off, (apm_rd(au, off) & ~mask) | (val & mask));
	spin_unlock_irqrestore(&au->lock, flags);
}

static inline u32 dmac_rd(struct t48s_audio *au, u32 off)
{
	return readl(au->dmac + off);
}

static inline void dmac_wr(struct t48s_audio *au, u32 off, u32 val)
{
	writel(val, au->dmac + off);
}

/* DW DMAC per-channel bit registers take a write-enable bit at ch + 8, so a
 * write changes this channel's bit and no other */
static inline void dmac_chbit(struct t48s_audio *au, u32 reg, int ch, int on)
{
	dmac_wr(au, reg, (1 << (ch + 8)) | (on ? (1 << ch) : 0));
}

static void dmac_stop(struct t48s_audio *au, int ch)
{
	int n = 100000;

	dmac_chbit(au, DMAC_CHEN, ch, 0);
	while ((dmac_rd(au, DMAC_CHEN) & (1 << ch)) && --n)
		cpu_relax();
}

static void dmac_quiet(struct t48s_audio *au, int ch)
{
	dmac_chbit(au, DMAC_MASK_TFR, ch, 0);
	dmac_chbit(au, DMAC_MASK_BLOCK, ch, 0);
	dmac_chbit(au, DMAC_MASK_SRCTRAN, ch, 0);
	dmac_chbit(au, DMAC_MASK_DSTTRAN, ch, 0);
	dmac_chbit(au, DMAC_MASK_ERR, ch, 0);
	dmac_stop(au, ch);
	dmac_wr(au, DMAC_CLEAR_TFR, 1 << ch);
	dmac_wr(au, DMAC_CLEAR_BLOCK, 1 << ch);
	dmac_wr(au, DMAC_CLEAR_SRCTRAN, 1 << ch);
	dmac_wr(au, DMAC_CLEAR_DSTTRAN, 1 << ch);
	dmac_wr(au, DMAC_CLEAR_ERR, 1 << ch);
}

/* ---- PCM ---- */

static const struct snd_pcm_hardware t48s_hw = {
	.info			= SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED |
				  SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_16000,
	.rate_min		= 16000,
	.rate_max		= 16000,
	.channels_min		= 1,
	.channels_max		= 1,
	.buffer_bytes_max	= MAX_BLKS * BLK_BYTES,
	.period_bytes_min	= BLK_BYTES,
	.period_bytes_max	= MAX_BLKS * BLK_BYTES / 2,
	.periods_min		= 2,
	.periods_max		= MAX_BLKS,
};

static int t48s_pcm_open(struct snd_pcm_substream *ss)
{
	struct t48s_codec *c = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	int rc;

	rt->hw = t48s_hw;
	/* periods are whole DMA blocks, and the ring is a whole number of periods */
	rc = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, BLK_BYTES);
	if (rc < 0)
		return rc;
	rc = snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
	if (rc < 0)
		return rc;
	c->ss = ss;
	return 0;
}

static int t48s_pcm_close(struct snd_pcm_substream *ss)
{
	struct t48s_codec *c = snd_pcm_substream_chip(ss);

	c->ss = NULL;
	return 0;
}

static int t48s_pcm_hw_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *hp)
{
	return snd_pcm_lib_malloc_pages(ss, params_buffer_bytes(hp));
}

static int t48s_pcm_hw_free(struct snd_pcm_substream *ss)
{
	return snd_pcm_lib_free_pages(ss);
}

static int t48s_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct t48s_codec *c = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	unsigned int bytes = frames_to_bytes(rt, rt->buffer_size);
	unsigned int per = frames_to_bytes(rt, rt->period_size) / BLK_BYTES;
	unsigned int i;

	if (!per || bytes % BLK_BYTES || bytes / BLK_BYTES > MAX_BLKS)
		return -EINVAL;
	c->nblk = bytes / BLK_BYTES;
	for (i = 0; i < c->nblk; i++) {
		struct lli *l = &c->lli[i];

		l->sar = c->d->fifo;
		l->dar = rt->dma_addr + i * BLK_BYTES;
		l->llp = c->lli_dma + ((i + 1) % c->nblk) * sizeof(struct lli);
		l->ctl_lo = RX_CTL_LO | ((i + 1) % per ? 0 : CTL_INT_EN);
		l->ctl_hi = BLK_WORDS;
		l->sstat = 0;
		l->dstat = 0;
		l->pad = 0;
	}
	/* before the ring: one block of stale FIFO contents, thrown away */
	c->lli[PRE_ITEM] = c->lli[0];
	c->lli[PRE_ITEM].dar = c->lli_dma + SCRAP_OFF;
	c->lli[PRE_ITEM].llp = c->lli_dma;
	c->lli[PRE_ITEM].ctl_lo = RX_CTL_LO;
	wmb();
	return 0;
}

static void t48s_start(struct t48s_codec *c)
{
	struct t48s_audio *au = c->au;
	int ch = c->d->dma_ch;
	u32 base = DMAC_CH(ch);

	dmac_quiet(au, ch);
	if (dmac_rd(au, DMAC_CFG) != 1)
		dmac_wr(au, DMAC_CFG, 1);	/* controller enable */
	dmac_wr(au, base + DMAC_SAR, c->d->fifo);
	dmac_wr(au, base + DMAC_DAR, c->lli_dma + SCRAP_OFF);
	dmac_wr(au, base + DMAC_LLP, c->lli_dma + PRE_ITEM * sizeof(struct lli));
	dmac_wr(au, base + DMAC_CTL_LO, CTL_LLP_EN);	/* first block comes from the list */
	dmac_wr(au, base + DMAC_CTL_HI, 0);
	dmac_wr(au, base + DMAC_CFG_LO, RX_CFG_LO);
	dmac_wr(au, base + DMAC_CFG_HI, c->d->cfg_hi);
	dmac_chbit(au, DMAC_MASK_BLOCK, ch, 1);
	dmac_chbit(au, DMAC_MASK_ERR, ch, 1);
	c->ovf_armed = 0;
	c->ovf_run = 0;
	c->starts++;
	c->running = 1;
	dmac_chbit(au, DMAC_CHEN, ch, 1);
}

static void t48s_stop(struct t48s_codec *c)
{
	struct t48s_audio *au = c->au;

	c->running = 0;
	apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) & ~c->d->ovf_bit);
	dmac_quiet(au, c->d->dma_ch);
}

static int t48s_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct t48s_codec *c = snd_pcm_substream_chip(ss);
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&c->au->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		t48s_start(c);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		t48s_stop(c);
		break;
	default:
		rc = -EINVAL;
	}
	spin_unlock_irqrestore(&c->au->lock, flags);
	return rc;
}

static snd_pcm_uframes_t t48s_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct t48s_codec *c = snd_pcm_substream_chip(ss);
	struct t48s_audio *au = c->au;
	struct snd_pcm_runtime *rt = ss->runtime;
	u32 base = DMAC_CH(c->d->dma_ch);
	u32 bytes = c->nblk * BLK_BYTES;
	u32 scrap = c->lli_dma + SCRAP_OFF;
	u32 llp, dar, blk, start, off;
	int tries = 4;

	/* the LLP register points at the item after the one being filled */
	do {
		llp = dmac_rd(au, base + DMAC_LLP);
		dar = dmac_rd(au, base + DMAC_DAR);
	} while (llp != dmac_rd(au, base + DMAC_LLP) && --tries);
	if (!tries)
		dar = 0;
	blk = (llp - c->lli_dma) / sizeof(struct lli);
	if (llp < c->lli_dma || blk >= c->nblk)
		return 0;
	if (dar >= scrap && dar <= scrap + BLK_BYTES)
		return 0;	/* still draining the FIFO */
	blk = (blk + c->nblk - 1) % c->nblk;
	start = blk * BLK_BYTES;
	/* finer than a block when the destination address agrees (it may already
	 * stand at the start of the next block while the next item is being loaded) */
	off = dar - rt->dma_addr;
	if (dar >= rt->dma_addr && off >= start && off <= start + BLK_BYTES)
		start = off % bytes;
	else if (dar == rt->dma_addr && blk == c->nblk - 1)
		start = 0;
	return bytes_to_frames(rt, start);
}

static struct snd_pcm_ops t48s_pcm_ops = {
	.open		= t48s_pcm_open,
	.close		= t48s_pcm_close,
	.ioctl		= snd_pcm_lib_ioctl,
	.hw_params	= t48s_pcm_hw_params,
	.hw_free	= t48s_pcm_hw_free,
	.prepare	= t48s_pcm_prepare,
	.trigger	= t48s_pcm_trigger,
	.pointer	= t48s_pcm_pointer,
};

/*
 * Block interrupt of one DMA0 channel: a period has been filled. The DMAC also
 * raises a few that no period-ending item asked for (measured: about 5 in 1000
 * alone, 50 in 1000 while the other codec records too, none when every item
 * interrupts). They are harmless: the pointer is exact, so ALSA sees a period
 * that is not complete yet and waits for the real one.
 */
static irqreturn_t t48s_dma_irq(int irq, void *data)
{
	struct t48s_codec *c = data;
	struct t48s_audio *au = c->au;
	int ch = c->d->dma_ch;
	unsigned long flags;
	int running, err;

	spin_lock_irqsave(&au->lock, flags);
	err = dmac_rd(au, DMAC_STATUS_ERR) & (1 << ch);
	dmac_wr(au, DMAC_CLEAR_BLOCK, 1 << ch);
	dmac_wr(au, DMAC_CLEAR_TFR, 1 << ch);
	dmac_wr(au, DMAC_CLEAR_ERR, 1 << ch);
	c->irqs++;
	if (err)
		c->dma_errors++;
	running = c->running;
	if (running && !c->ovf_armed) {
		/* the FIFO sat full before the start; from now on an overflow is real */
		c->ovf_armed = 1;
		apm_wr(au, APM_IRQSTAT, c->d->ovf_bit);
		apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) | c->d->ovf_bit);
	}
	spin_unlock_irqrestore(&au->lock, flags);

	if (err)
		pr_warn_ratelimited(DRV_NAME ": %s: DMA error\n", c->d->name);
	if (running && c->ss)
		snd_pcm_period_elapsed(c->ss);
	return IRQ_HANDLED;
}

/* APM interrupt: only the ADC FIFO overflow bits of running captures are enabled */
static irqreturn_t t48s_apm_irq(int irq, void *data)
{
	struct t48s_audio *au = data;
	unsigned long flags;
	u32 st;
	int i;

	spin_lock_irqsave(&au->lock, flags);
	st = apm_rd(au, APM_IRQSTAT) & apm_rd(au, APM_IRQEN);
	for (i = 0; i < 2; i++) {
		struct t48s_codec *c = &au->codec[i];

		if (!(st & c->d->ovf_bit))
			continue;
		apm_wr(au, APM_IRQSTAT, c->d->ovf_bit);
		c->overflows++;
		if (++c->ovf_run >= OVF_LIMIT) {
			apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) & ~c->d->ovf_bit);
			pr_warn(DRV_NAME ": %s: ADC FIFO keeps overflowing, no longer counting\n",
				c->d->name);
		}
	}
	st &= ~(APM_IRQ_ADC_OVF_A | APM_IRQ_ADC_OVF_B);
	if (st) {	/* nothing else should be enabled; make sure it cannot storm */
		apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) & ~st);
		pr_warn(DRV_NAME ": unexpected APM interrupt 0x%x, disabled\n", st);
	}
	spin_unlock_irqrestore(&au->lock, flags);
	return IRQ_HANDLED;
}

/* ---- mixer controls ---- */

enum { CTL_SWITCH, CTL_SOURCE, CTL_PREAMP, CTL_DGAIN, CTL_BIAS_A, CTL_BIAS_B };
#define CTL_PV(codec, kind)	(((codec) << 8) | (kind))
#define CTL_CODEC(kc)		(((kc)->private_value >> 8) & 1)
#define CTL_KIND(kc)		((kc)->private_value & 0xff)

/* digital gain: step 0 mutes, step n is (n - 36) dB; the register holds a 15-bit
 * gain and a shift, as ApmHW_DigitalGainSet writes them */
static u32 dgain_bits(int step)
{
	u32 g = DGAIN_UNITY << 8, shift = 3;
	int i;

	if (step <= 0)
		return shift << 24;
	for (i = DGAIN_0DB_STEP; i < step; i++)
		g = (u32)(((u64)g * 73533) >> 16);	/* +1 dB */
	for (i = DGAIN_0DB_STEP; i > step; i--)
		g = (u32)(((u64)g * 58409) >> 16);	/* -1 dB */
	g = (g + 128) >> 8;
	while (g >= 0x8000) {
		g >>= 1;
		shift++;
	}
	return (shift << 24) | g;
}

static int dgain_step_of(u32 reg)
{
	u32 want = (reg & 0x7fff) << (((reg >> 24) & 0xf) > 3 ? ((reg >> 24) & 0xf) - 3 : 0);
	u32 best = ~0;
	int step, found = 0;

	if (!(reg & 0xffff))
		return 0;
	for (step = 1; step <= DGAIN_MAX_STEP; step++) {
		u32 b = dgain_bits(step);
		u32 v = (b & 0x7fff) << ((b >> 24) - 3);
		u32 diff = v > want ? v - want : want - v;

		if (diff < best) {
			best = diff;
			found = step;
		}
	}
	return found;
}

static int t48s_ctl_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	static const char * const src[2] = { "Input A", "Input B" };

	ui->count = 1;
	switch (CTL_KIND(kc)) {
	case CTL_SOURCE:
		return snd_ctl_enum_info(ui, 1, 2, src);
	case CTL_PREAMP:
		ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		ui->value.integer.max = PGA_MAX_STEP;
		break;
	case CTL_DGAIN:
		ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		ui->value.integer.max = DGAIN_MAX_STEP;
		break;
	default:
		ui->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
		ui->value.integer.max = 1;
	}
	ui->value.integer.min = 0;
	return 0;
}

static int t48s_ctl_read(struct t48s_codec *c, int kind)
{
	struct t48s_audio *au = c->au;
	const struct t48s_codec_desc *d = c->d;
	int v;

	switch (kind) {
	case CTL_SWITCH:
		return (apm_rd(au, APM_ANA) & d->adc_mask) == d->adc_on;
	case CTL_SOURCE:
		return !!(apm_rd(au, d->afe_reg) & APM_AFE_INPUT_B);
	case CTL_PREAMP:
		v = ((apm_rd(au, d->afe_reg) & APM_AFE_PGA_MASK) >> APM_AFE_PGA_SHIFT) / 3;
		return min(v, PGA_MAX_STEP);
	case CTL_DGAIN:
		return c->dgain_step;
	case CTL_BIAS_A:
		return !(apm_rd(au, d->micdet_reg) & APM_MICDET_BIASA_OFF);
	default:
		return !(apm_rd(au, APM_ANA) & d->biasb_off);
	}
}

static int t48s_ctl_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct t48s_audio *au = snd_kcontrol_chip(kc);
	int v = t48s_ctl_read(&au->codec[CTL_CODEC(kc)], CTL_KIND(kc));

	if (CTL_KIND(kc) == CTL_SOURCE)
		uc->value.enumerated.item[0] = v;
	else
		uc->value.integer.value[0] = v;
	return 0;
}

static int t48s_ctl_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct t48s_audio *au = snd_kcontrol_chip(kc);
	struct t48s_codec *c = &au->codec[CTL_CODEC(kc)];
	const struct t48s_codec_desc *d = c->d;
	int kind = CTL_KIND(kc);
	int v, max = 1;

	if (kind == CTL_SOURCE)
		v = uc->value.enumerated.item[0];
	else
		v = uc->value.integer.value[0];
	if (kind == CTL_PREAMP)
		max = PGA_MAX_STEP;
	else if (kind == CTL_DGAIN)
		max = DGAIN_MAX_STEP;
	if (v < 0 || v > max)
		return -EINVAL;
	if (v == t48s_ctl_read(c, kind))
		return 0;

	switch (kind) {
	case CTL_SWITCH:
		apm_update(au, APM_ANA, d->adc_mask, v ? d->adc_on : 0);
		break;
	case CTL_SOURCE:
		apm_update(au, d->afe_reg, APM_AFE_INPUT_B, v ? APM_AFE_INPUT_B : 0);
		break;
	case CTL_PREAMP:
		apm_update(au, d->afe_reg, APM_AFE_PGA_MASK, (v * 3) << APM_AFE_PGA_SHIFT);
		break;
	case CTL_DGAIN:
		apm_update(au, d->dgain_reg, ~DGAIN_KEEP, dgain_bits(v));
		c->dgain_step = v;
		break;
	case CTL_BIAS_A:
		apm_update(au, d->micdet_reg, APM_MICDET_BIASA_OFF, v ? 0 : APM_MICDET_BIASA_OFF);
		break;
	default:
		apm_update(au, APM_ANA, d->biasb_off, v ? 0 : d->biasb_off);
	}
	return 1;
}

static const DECLARE_TLV_DB_SCALE(t48s_tlv_preamp, 0, 300, 0);
static const DECLARE_TLV_DB_SCALE(t48s_tlv_dgain, -3600, 100, 1);

#define T48S_CTL(cname, codec, kind) {					\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = cname,		\
	.info = t48s_ctl_info, .get = t48s_ctl_get, .put = t48s_ctl_put, \
	.private_value = CTL_PV(codec, kind) }
#define T48S_VOL(cname, codec, kind, tlvp) {				\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = cname,		\
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |			\
		  SNDRV_CTL_ELEM_ACCESS_TLV_READ,			\
	.info = t48s_ctl_info, .get = t48s_ctl_get, .put = t48s_ctl_put, \
	.tlv = { .p = (tlvp) }, .private_value = CTL_PV(codec, kind) }

static struct snd_kcontrol_new t48s_ctls[] = {
	T48S_CTL("Codec A Capture Switch", 0, CTL_SWITCH),
	T48S_CTL("Codec A Capture Source", 0, CTL_SOURCE),
	T48S_VOL("Codec A Preamp Capture Volume", 0, CTL_PREAMP, t48s_tlv_preamp),
	T48S_VOL("Codec A Capture Volume", 0, CTL_DGAIN, t48s_tlv_dgain),
	T48S_CTL("Codec A Mic Bias A Switch", 0, CTL_BIAS_A),
	T48S_CTL("Codec A Mic Bias B Switch", 0, CTL_BIAS_B),
	T48S_CTL("Codec B Capture Switch", 1, CTL_SWITCH),
	T48S_CTL("Codec B Capture Source", 1, CTL_SOURCE),
	T48S_VOL("Codec B Preamp Capture Volume", 1, CTL_PREAMP, t48s_tlv_preamp),
	T48S_VOL("Codec B Capture Volume", 1, CTL_DGAIN, t48s_tlv_dgain),
	T48S_CTL("Codec B Mic Bias A Switch", 1, CTL_BIAS_A),
	T48S_CTL("Codec B Mic Bias B Switch", 1, CTL_BIAS_B),
};

/* ---- /proc/asound/card0/t48s ---- */

static void t48s_proc_read(struct snd_info_entry *entry, struct snd_info_buffer *buf)
{
	struct t48s_audio *au = entry->private_data;
	static const u16 regs[] = { 0x004, 0x008, 0x00c, 0x020, 0x024, 0x02c, 0x040, 0x044,
				    0x050, 0x054, 0x05c, 0x070, 0x074, 0x120, 0x124, 0x12c,
				    0x134, 0x138, 0x13c, 0x42c, 0x52c };
	int i;

	for (i = 0; i < 2; i++) {
		struct t48s_codec *c = &au->codec[i];

		snd_iprintf(buf, "%s: DMA0 channel %d, %s, starts %lu, period interrupts %lu, "
			    "DMA errors %lu, ADC FIFO overflows %lu\n",
			    c->d->name, c->d->dma_ch, c->running ? "running" : "stopped",
			    c->starts, c->irqs, c->dma_errors, c->overflows);
	}
	snd_iprintf(buf, "DMA0: enable 0x%x, channels running 0x%02x\n",
		    dmac_rd(au, DMAC_CFG), dmac_rd(au, DMAC_CHEN) & 0xff);
	snd_iprintf(buf, "APM registers:\n");
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		snd_iprintf(buf, " %03x: %08x\n", regs[i], apm_rd(au, regs[i]));
}

/* ---- init ---- */

static void t48s_halt_dsp(void)
{
	void __iomem *chipc = ioremap(CHIPC_PHYS, CHIPC_SIZE);

	if (!chipc)
		return;
	/* after a kexec boot Yealink's voice DSP may still be running with no host;
	 * nothing here uses it. Same writes as vpmHw_Halt in Yealink's kernel. */
	if (!(readl(chipc + CHIPC_VPM_HALT) & 1) ||
	    (readl(chipc + CHIPC_CLKEN) & CHIPC_CLKEN_VPM)) {
		writel(1, chipc + CHIPC_VPM_HALT);
		writel(readl(chipc + CHIPC_CLKEN) & ~CHIPC_CLKEN_VPM, chipc + CHIPC_CLKEN);
		pr_info(DRV_NAME ": voice DSP halted\n");
	}
	iounmap(chipc);
}

static int __init t48s_audio_init(void)
{
	struct platform_device *pdev;
	struct snd_card *card;
	struct t48s_audio *au;
	int i, rc;

	pdev = platform_device_register_simple(DRV_NAME, -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	rc = snd_card_create(-1, "T48S", THIS_MODULE, sizeof(*au), &card);
	if (rc < 0)
		goto fail_pdev;
	au = card->private_data;
	au->card = card;
	au->dev = &pdev->dev;
	spin_lock_init(&au->lock);
	snd_card_set_dev(card, &pdev->dev);
	strlcpy(card->driver, DRV_NAME, sizeof(card->driver));
	strlcpy(card->shortname, "T48S", sizeof(card->shortname));
	strlcpy(card->longname, "Yealink T48S microphones (BCM11188 APM codecs A and B)",
		sizeof(card->longname));
	strlcpy(card->mixername, "BCM11188 APM", sizeof(card->mixername));

	rc = -ENOMEM;
	au->apm = ioremap(APM_PHYS, APM_SIZE);
	au->dmac = ioremap(DMA0_PHYS, DMA0_SIZE);
	if (!au->apm || !au->dmac)
		goto fail_card;

	t48s_halt_dsp();

	/* Yealink leaves the four FIFO error interrupts enabled; with no DMA running
	 * they are all pending. Enable none until a capture runs. */
	apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) & ~0xff);
	for (i = 0; i < 2; i++) {
		au->codec[i].d = &t48s_codecs[i];
		au->codec[i].au = au;
	}
	rc = request_irq(IRQ_APM, t48s_apm_irq, 0, "t48s_audio apm", au);
	if (rc)
		goto fail_card;

	for (i = 0; i < 2; i++) {
		struct t48s_codec *c = &au->codec[i];
		struct snd_pcm *pcm;

		c->dgain_step = dgain_step_of(apm_rd(au, c->d->dgain_reg));
		rc = -ENOMEM;
		c->lli = dma_alloc_coherent(&pdev->dev, LLI_BYTES, &c->lli_dma, GFP_KERNEL);
		if (!c->lli)
			goto fail_card;
		dmac_quiet(au, c->d->dma_ch);
		rc = request_irq(c->d->irq, t48s_dma_irq, 0,
				 i ? "t48s_audio B" : "t48s_audio A", c);
		if (rc)
			goto fail_card;

		rc = snd_pcm_new(card, i ? "T48S codec B" : "T48S codec A", i, 0, 1, &pcm);
		if (rc < 0)
			goto fail_card;
		pcm->private_data = c;
		strlcpy(pcm->name, i ? "T48S codec B" : "T48S codec A", sizeof(pcm->name));
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &t48s_pcm_ops);
		snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV, &pdev->dev,
						      MAX_BLKS * BLK_BYTES, MAX_BLKS * BLK_BYTES);

		if (apm_rd(au, c->d->rate_reg) != APM_RATE_16K)
			pr_warn(DRV_NAME ": %s: sample rate register is 0x%08x, not the 16 kHz setting\n",
				c->d->name, apm_rd(au, c->d->rate_reg));
	}
	if ((apm_rd(au, APM_CTL) >> 28) != 0xf)
		pr_warn(DRV_NAME ": APM codecs not enabled (control 0x%08x): cold start is not supported yet\n",
			apm_rd(au, APM_CTL));

	for (i = 0; i < ARRAY_SIZE(t48s_ctls); i++) {
		rc = snd_ctl_add(card, snd_ctl_new1(&t48s_ctls[i], au));
		if (rc < 0)
			goto fail_card;
	}
	{
		struct snd_info_entry *entry;

		if (!snd_card_proc_new(card, "t48s", &entry))
			snd_info_set_text_ops(entry, au, t48s_proc_read);
	}

	rc = snd_card_register(card);
	if (rc < 0)
		goto fail_card;
	pr_info(DRV_NAME ": capture from codec A (hw:0,0) and codec B (hw:0,1), 16 kHz mono\n");
	return 0;

fail_card:
	pr_err(DRV_NAME ": init failed (%d)\n", rc);
	return rc;	/* this is built in: the few allocations are left behind */
fail_pdev:
	platform_device_unregister(pdev);
	return rc;
}
late_initcall(t48s_audio_init);
MODULE_LICENSE("GPL");
