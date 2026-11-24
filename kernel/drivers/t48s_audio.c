// SPDX-License-Identifier: GPL-2.0
/*
 * t48s_audio: microphones, earpieces and loudspeaker of the Yealink SIP-T48S
 * (Broadcom BCM11188).
 *
 * The SoC's APM block (0x30600000) holds two audio codecs, "A" and "B", each
 * one ADC and one DAC running at 16 kHz. Each ADC has a two-way analog input
 * switch (inputs "A" and "B") and a preamp; each DAC has two analog outputs
 * ("A" and "B"). Every converter has a small FIFO that is a request line on
 * DMA controller 0 (a Synopsys DW AHB DMAC at 0x30200000):
 *   channel 2  codec A DAC     channel 3  codec A ADC
 *   channel 4  codec B DAC     channel 5  codec B ADC
 * No DSP and no vendor module is involved (the voice DSP is halted here if it
 * is still running).
 *
 * On this phone (docs/audio.md, daytime tests 1 and 2):
 *   codec B  input B = handset microphone, output B = handset earpiece,
 *            output A = headset
 *   codec A  input A = base (speakerphone) microphone, output A = loudspeaker,
 *            through an external amplifier enabled by GPIO31
 *
 * ALSA view: one card, "T48S", with two devices, each capture and playback:
 *   hw:0,0  codec A      hw:0,1  codec B
 * S16_LE, mono, 16000 Hz. Periods are whole 5 ms blocks (80 frames).
 *
 * Mixer controls, per codec ("Codec A ...", "Codec B ..."):
 *   Capture Switch          ADC analog front end power (Broadcom uses this as
 *                           the microphone mute)
 *   Capture Source          analog input A or B
 *   Preamp Capture Volume   0 to 42 dB in 3 dB steps
 *   Capture Volume          ADC digital gain, mute or -35 to +12 dB
 *   Mic Bias A Switch       bias supply of input A (tied to mic detection)
 *   Mic Bias B Switch       bias supply of input B
 *   Output A Playback Switch, Output B Playback Switch
 *                           the analog output stage: on, or muted and off
 *   Output A Playback Volume, Output B Playback Volume
 *                           analog output gain, -21 to 0 dB in 3 dB steps
 *   Playback Volume         DAC digital gain, mute or -35 to +6 dB
 *   Sidetone Switch         microphone fed to this codec's own outputs
 *   Sidetone Volume         mute or -35 to 0 dB
 * and one for the board:
 *   Speaker Amp Switch      GPIO31, the loudspeaker amplifier
 *
 * QUIET BY DEFAULT. Yealink's firmware idles with codec B's output B on and
 * its sidetone on, and a kexec boot inherits that. At start-up this driver
 * takes the output side over: amplifier off, sidetone off, all four outputs
 * muted and switched off, analog output gains at their lowest (-21 dB) and
 * both DAC digital gains at -20 dB. Nothing can be heard until user space
 * switches an output on, and the amplifier is only ever switched on by its
 * control. Switching the loudspeaker output off switches the amplifier off.
 *
 * No clicks: an output is muted before it is switched off and switched on
 * before it is unmuted, and the mute itself is the codec's own 40 ms ramp
 * (left as Yealink set it). A playback stream starts from a block of zeros.
 * When it stops, is dropped or underruns, the DMA is steered to a 5 ms fade
 * from the last sample played down to zero and then round a block of zeros,
 * so the DAC is never left holding a level or starved in mid-wave. The DMA
 * channel is only stopped when the stream is closed.
 *
 * COLD START. After a kexec from Yealink's kernel the APM block is running
 * and is used as found. When it is not (the phone came straight from U-Boot,
 * which does nothing with audio: the block is not clocked, and reading one of
 * its registers stops the kernel with an external abort), t48s_apm_cold_start()
 * brings it up: the two APM clocks, the audio channel enables and the block's
 * soft reset in chip control, Broadcom's ApmHw_init register table, 16 kHz on
 * both codecs, the codec enables, and the analog side as Yealink idles. The
 * equalisers stay off (flat); Yealink loads its own tuning into them, which a
 * phone that started from Yealink still has.
 *
 * Whether the block is on is read from chip control, never from the block.
 * The first accesses to the block are made with the external abort caught
 * (t48s_guard), so a block that still does not answer costs the sound and not
 * the boot: the driver then says so and registers nothing.
 *
 * To test the cold start without a cold boot (no stream may be open; stop the
 * audio service first):
 *   echo 1 > /sys/module/t48s_audio/parameters/cold
 * puts the block back into the state a power-on leaves (held in reset, clocks
 * gated, audio channels disabled, the audio DMA controller off), shows that a
 * read is caught, and cold-starts it. See docs/audio.md, "Cold start".
 *
 * DMA: each channel runs a ring of linked-list items, one per 5 ms block,
 * straight out of or into the ALSA buffer, with the control words Yealink's
 * kernel was seen using. The item that ends a period raises the block
 * interrupt. The ADC never stops, so while nothing records its FIFO sits full
 * of old samples (64 of them, measured); each capture start first reads one
 * block into a scrap buffer to get rid of those. Our kernel's DMA framework
 * is patched to leave controller 0 alone (kernel/build.sh); this driver
 * programs only its own four channels' registers and the per-channel bits of
 * the shared ones, so channels 0, 1, 6 and 7 are never touched.
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
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/info.h>
#include <mach/irqs.h>

#include <linux/moduleparam.h>
#include <asm/system_misc.h>

#define DRV_NAME	"t48s_audio"

#define APM_PHYS	0x30600000
#define APM_SIZE	0x1000
#define DMA0_PHYS	0x30200000
#define DMA0_SIZE	0x1000
#define CHIPC_PHYS	0x80000000
#define CHIPC_SIZE	0x4000
#define GPIO0_PHYS	0x80004000	/* GPIO0-31 data, one bit per pin */
#define GPIO0_SIZE	0x1000
#define GPIO_SPK_AMP	0x80000000	/* GPIO31: loudspeaker amplifier, high = on */

/* chip control: the APM's clocks, enables and reset; the voice DSP (VPM) */
#define CHIPC_APM100_CLK	0x44		/* 200 MHz / 2 */
#define CHIPC_APM100_CLK_ON	0x00010002	/* as Yealink runs it: divide by 2, output on */
#define CHIPC_APM200_CLK	0x58		/* PLL2 channel 3 */
#define CHIPC_APM200_CLK_ON	0x00010009	/* as Yealink runs it: divide by 9, output on */
#define CHIPC_CLK_OUTPUT	0x00010000
#define CHIPC_BUSCLK		0x5c
#define CHIPC_BUSCLK_DMAC0	0x00000020
#define CHIPC_AUDIO_EN		0x7c
#define CHIPC_AUDIO_EN_CLOCKS	0x00000090	/* APM and NTP clocks */
#define CHIPC_AUDIO_EN_CODECS	0x00000007	/* all, channel A, channel B */
#define CHIPC_AUDIO_EN_OURS	(CHIPC_AUDIO_EN_CLOCKS | CHIPC_AUDIO_EN_CODECS)
#define CHIPC_SOFTRESET2	0x84
#define CHIPC_SOFTRESET2_APM	0x00002001
#define CHIPC_PLL2_PREDIV	0xa8		/* PLL2, which the 200 MHz comes from: */
#define CHIPC_PLL2_PREDIV_OK	0x02004811	/* as U-Boot sets it and Yealink leaves it */
#define CHIPC_GPIOMUX3		0x124		/* GPIO24-31, 4 bits per pin, 0 = plain GPIO */
#define GPIO_DIR		0x400		/* 1 = output */
#define CHIPC_CLKEN		0x10
#define CHIPC_CLKEN_VPM		0x00010000
#define CHIPC_VPM_HALT		0x301c

/* APM */
#define APM_FLUSH		0x000	/* FIFO flush strobes */
#define APM_IRQSTAT		0x004	/* write 1 to clear */
#define APM_IRQEN		0x008
#define APM_IRQ_DAC_UNF_A	0x10
#define APM_IRQ_DAC_UNF_B	0x20
#define APM_IRQ_ADC_OVF_A	0x40
#define APM_IRQ_ADC_OVF_B	0x80
#define APM_IRQ_FIFO		0xf0
#define APM_CTL			0x00c	/* top four bits: codec enables */
#define APM_CTL_INIT		0x00810204
#define APM_CTL_CODECS		0xf0000000
#define APM_CODEC_A		0x020	/* codec control; bits 31 and 30 hold A and B
					 * until they are enabled, 26 and 27 switch the
					 * two equalisers on */
#define APM_CODEC_B		0x050
#define APM_CODEC_HOLD_AB	0xc0000000
#define APM_RATE_LOAD		0x080	/* value for the strobes in APM_RATE_CTL */
#define APM_RATE_LOAD_IDLE	0x53e2d624
#define APM_RATE_LOAD_16K	0x14f8b589
#define APM_RATE_CTL		0x084
#define APM_FIFO_CLR_A		0x104	/* written with all ones before the enable */
#define APM_FIFO_CLR_B		0x10c
#define APM_FIFO_CLR		0x114
#define APM_COEF_DATA		0x010	/* filter coefficient memory: value, */
#define APM_COEF_ADDR		0x014	/* then index, bank and a write strobe */
#define APM_COEF_STROBE		0x100
#define APM_COEF_SIDETONE_A	0x00100000
#define APM_COEF_SIDETONE_B	0x08000000
#define APM_SIDETONE_A		0x024
#define APM_SIDETONE_B		0x054
#define APM_SIDETONE_EN		0x00008000
#define APM_DAC_DGAIN_A		0x02c	/* bits 16-18 shift, low 16 bits gain */
#define APM_DAC_DGAIN_B		0x05c
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
#define APM_OUT_MUTE		0x134	/* bits 16-19: output mutes (ramped); the
					 * low 16 bits set the ramps, left alone */
#define APM_OUT_MUTE_ALL	0x000f0000
#define APM_OUT_MODE		0x138	/* bits 12-19: output modes, 0 = on, 3 = off;
					 * bits 20-31: output attenuation, 3 dB a step */
#define APM_OUT_MODE_ALL	0x000ff000
#define APM_OUT_GAIN_ALL	0xfff00000
#define APM_OUT_RAMPS		0x00003333	/* in APM_OUT_MUTE: each output's mute ramps
						 * over 40 ms, as Yealink sets it */
#define APM_DAC			0x13c
#define APM_DAC_POWERDOWN	0x00000600	/* bit 9 codec B, bit 10 codec A */
#define APM_DAC_RAMP_COSINE	0x60000000
#define APM_MICDET_A		0x42c	/* bit 1: input A mic bias off */
#define APM_MICDET_B		0x52c
#define APM_MICDET_BIASA_OFF	0x00000002

#define ADC_UNITY		0x29f1	/* 0 dB at 16 kHz (Broadcom's scale) */
#define ADC_DGAIN_KEEP		0xf0ff0000
#define DAC_UNITY		0x7fff
#define DAC_DGAIN_KEEP		0xfff80000
#define DGAIN_0DB_STEP		36	/* 0 = mute, n = n - 36 dB */
#define ADC_MAX_STEP		48	/* +12 dB */
#define DAC_MAX_STEP		42	/* +6 dB */
#define DAC_START_STEP		16	/* -20 dB until user space says otherwise */
#define ST_MAX_STEP		36	/* sidetone: 0 dB at most */
#define ST_START_STEP		18	/* -18 dB, and switched off */
#define ST_UNITY		0x80000
#define ST_COEFS		28
#define PGA_MAX_STEP		14	/* 3 dB each */
#define OUT_MAX_STEP		7	/* 3 dB each, 7 = 0 dB */
#define MUTE_RAMP_MS		100	/* the codec's own ramp is 40 ms */

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
 * Seen on Yealink's running channels. ADC: both list pointers enabled, device
 * to memory with the DMAC counting, 32-bit reads from the FIFO in bursts of
 * four (0x1a204827). DAC: memory to device, 32-bit writes to the FIFO in
 * bursts of four (0x00904835); Yealink queues those a block at a time, here
 * they run from a list like the ADC's, so the two list-pointer bits are
 * added. Bit 0 (interrupt enable) is set on the item that ends a period.
 */
#define RX_CTL_LO		0x1a204826
#define TX_CTL_LO		0x18904834
#define CTL_INT_EN		0x00000001
#define CTL_LLP_EN		0x18000000
#define DMA_CFG_LO		0x000000c0

#define BLK_WORDS		40	/* 32-bit FIFO words per block */
/* The DMAC counts a block in units of the source width. The ADC's control word
 * reads 32-bit units from the FIFO; the DAC's (as Yealink has it) takes memory
 * in 64-bit units, so its count is half (measured: with 40 a block lasts 10 ms). */
#define RX_BLK_COUNT		BLK_WORDS
#define TX_BLK_COUNT		(BLK_WORDS / 2)
#define BLK_BYTES		160	/* 80 samples, 5 ms */
#define BLK_FRAMES		80
#define MAX_BLKS		512	/* 2.56 s */
/* after the ring: one more list item, and two blocks of memory. Capture uses
 * the item and the first block to drain the stale FIFO at each start.
 * Playback uses the item for the block of zeros it idles on, and the second
 * block for the fade to zero when a stream stops. */
#define AUX_ITEM		MAX_BLKS
#define AUX_OFF			((MAX_BLKS + 1) * sizeof(struct lli))
#define FADE_OFF		(AUX_OFF + BLK_BYTES)
#define LLI_BYTES		(AUX_OFF + 2 * BLK_BYTES)
#define FIFO_ERR_LIMIT		100	/* FIFO error interrupts per run before giving up */

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

struct t48s_dir_desc {
	int dma_ch;
	int irq;
	u32 fifo;		/* physical */
	u32 cfg_hi;		/* FIFO mode, request line */
	u32 err_bit;		/* in APM_IRQSTAT / APM_IRQEN */
};

struct t48s_codec_desc {
	const char *name;
	struct t48s_dir_desc dir[2];	/* by SNDRV_PCM_STREAM_*: playback, capture */
	u32 adc_dgain_reg, dac_dgain_reg, rate_reg, afe_reg, micdet_reg;
	u32 sidetone_reg, sidetone_coef;
	u32 adc_mask, adc_on, biasb_off;
};

static const struct t48s_codec_desc t48s_codecs[2] = {
	{ "codec A",
	  { { 2, IRQ_DMA0C2, 0x30680000, 0x1806, APM_IRQ_DAC_UNF_A },
	    { 3, IRQ_DMA0C3, 0x30670000, 0x0106, APM_IRQ_ADC_OVF_A } },
	  APM_ADC_DGAIN_A, APM_DAC_DGAIN_A, APM_RATE_A, APM_AFE_A, APM_MICDET_A,
	  APM_SIDETONE_A, APM_COEF_SIDETONE_A,
	  APM_ANA_ADC_A, APM_ANA_ADC_A_ON, APM_ANA_BIASB_OFF_A },
	{ "codec B",
	  { { 4, IRQ_DMA0C4, 0x306a0000, 0x2806, APM_IRQ_DAC_UNF_B },
	    { 5, IRQ_DMA0C5, 0x30690000, 0x0206, APM_IRQ_ADC_OVF_B } },
	  APM_ADC_DGAIN_B, APM_DAC_DGAIN_B, APM_RATE_B, APM_AFE_B, APM_MICDET_B,
	  APM_SIDETONE_B, APM_COEF_SIDETONE_B,
	  APM_ANA_ADC_B, APM_ANA_ADC_B_ON, APM_ANA_BIASB_OFF_B },
};

/* the four analog outputs, index codec * 2 + output */
struct t48s_out_desc {
	u32 mute;		/* in APM_OUT_MUTE */
	int mode_shift;		/* two bits in APM_OUT_MODE */
	int gain_shift;		/* three bits in APM_OUT_MODE */
};

static const struct t48s_out_desc t48s_outs[4] = {
	{ 0x00040000, 16, 26 },		/* codec A output A: loudspeaker */
	{ 0x00080000, 18, 29 },		/* codec A output B */
	{ 0x00010000, 12, 20 },		/* codec B output A: headset */
	{ 0x00020000, 14, 23 },		/* codec B output B: handset earpiece */
};
#define OUT_SPEAKER		0

struct t48s_audio;
struct t48s_codec;

struct t48s_stream {
	const struct t48s_dir_desc *d;
	struct t48s_codec *codec;
	int play;
	struct snd_pcm_substream *ss;
	struct lli *lli;
	dma_addr_t lli_dma;
	unsigned int nblk;
	int running;
	int dma_on;		/* playback: the channel runs, on the ring or on zeros */
	unsigned long stopped;	/* playback: jiffies of the last stop */
	int err_armed;		/* FIFO error interrupt enabled for this run */
	unsigned int err_run;	/* FIFO errors in this run */

	/* counters, for /proc/asound/card0/t48s */
	unsigned long irqs, fifo_errors, dma_errors, starts;
};

struct t48s_codec {
	const struct t48s_codec_desc *d;
	struct t48s_audio *au;
	struct t48s_stream st[2];	/* by SNDRV_PCM_STREAM_* */
	int adc_step, dac_step, sidetone_step;
};

struct t48s_audio {
	struct snd_card *card;
	struct device *dev;
	void __iomem *apm;
	void __iomem *dmac;
	void __iomem *gpio;
	void __iomem *chipc;
	int cold_starts;	/* times this driver brought the block up itself */
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

/* the loudspeaker amplifier. The GPIO data register has no set/clear form and
 * is shared with the lamps, keypad and backlight drivers, which change it the
 * same way: read, change, write with interrupts off (one CPU). */
static void t48s_amp(struct t48s_audio *au, int on)
{
	unsigned long flags;
	u32 v;

	local_irq_save(flags);
	v = readl(au->gpio);
	writel(on ? v | GPIO_SPK_AMP : v & ~GPIO_SPK_AMP, au->gpio);
	local_irq_restore(flags);
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

static struct t48s_stream *t48s_stream_of(struct snd_pcm_substream *ss)
{
	struct t48s_codec *c = snd_pcm_substream_chip(ss);

	return &c->st[ss->stream];
}

static int t48s_pcm_open(struct snd_pcm_substream *ss)
{
	struct t48s_stream *s = t48s_stream_of(ss);
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
	s->ss = ss;
	return 0;
}

static int t48s_pcm_close(struct snd_pcm_substream *ss)
{
	t48s_stream_of(ss)->ss = NULL;
	return 0;
}

static int t48s_pcm_hw_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *hp)
{
	return snd_pcm_lib_malloc_pages(ss, params_buffer_bytes(hp));
}

/* playback: give a stream that has just stopped time to reach its block of
 * zeros (the block in hand, then the fade: 10 ms) before the ring is reused */
static void t48s_play_settle(struct t48s_stream *s)
{
	if (s->play && s->dma_on && time_before(jiffies, s->stopped + msecs_to_jiffies(30)))
		msleep(20);
}

static int t48s_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct t48s_stream *s = t48s_stream_of(ss);
	struct t48s_audio *au = s->codec->au;
	unsigned long flags;

	if (s->play && s->dma_on) {
		/* the DAC has been fed zeros since the stream stopped; now let it idle */
		t48s_play_settle(s);
		spin_lock_irqsave(&au->lock, flags);
		dmac_quiet(au, s->d->dma_ch);
		s->dma_on = 0;
		spin_unlock_irqrestore(&au->lock, flags);
	}
	return snd_pcm_lib_free_pages(ss);
}

static int t48s_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct t48s_stream *s = t48s_stream_of(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	unsigned int bytes = frames_to_bytes(rt, rt->buffer_size);
	unsigned int per = frames_to_bytes(rt, rt->period_size) / BLK_BYTES;
	struct lli *aux = &s->lli[AUX_ITEM];
	unsigned int i;

	if (!per || bytes % BLK_BYTES || bytes / BLK_BYTES > MAX_BLKS)
		return -EINVAL;
	t48s_play_settle(s);
	s->nblk = bytes / BLK_BYTES;
	for (i = 0; i < s->nblk; i++) {
		struct lli *l = &s->lli[i];
		u32 mem = rt->dma_addr + i * BLK_BYTES;

		l->sar = s->play ? mem : s->d->fifo;
		l->dar = s->play ? s->d->fifo : mem;
		l->llp = s->lli_dma + ((i + 1) % s->nblk) * sizeof(struct lli);
		l->ctl_lo = (s->play ? TX_CTL_LO : RX_CTL_LO) | ((i + 1) % per ? 0 : CTL_INT_EN);
		l->ctl_hi = s->play ? TX_BLK_COUNT : RX_BLK_COUNT;
		l->sstat = 0;
		l->dstat = 0;
		l->pad = 0;
	}
	if (s->play) {
		/* the block of zeros: it may be going round already (its own successor) */
		aux->sar = s->lli_dma + AUX_OFF;
		aux->dar = s->d->fifo;
		aux->ctl_lo = TX_CTL_LO;
		aux->ctl_hi = TX_BLK_COUNT;
		if (!s->dma_on)
			aux->llp = s->lli_dma + AUX_ITEM * sizeof(struct lli);
	} else {
		/* before the ring: one block of stale FIFO contents, thrown away */
		*aux = s->lli[0];
		aux->dar = s->lli_dma + AUX_OFF;
		aux->llp = s->lli_dma;
		aux->ctl_lo = RX_CTL_LO;
	}
	wmb();
	return 0;
}

/* start the channel on the extra list item: for capture the scrap block that
 * leads into the ring, for playback the block of zeros */
static void t48s_dma_go(struct t48s_stream *s)
{
	struct t48s_audio *au = s->codec->au;
	int ch = s->d->dma_ch;
	u32 base = DMAC_CH(ch);
	u32 mem = s->lli_dma + AUX_OFF;

	dmac_quiet(au, ch);
	if (dmac_rd(au, DMAC_CFG) != 1)
		dmac_wr(au, DMAC_CFG, 1);	/* controller enable */
	dmac_wr(au, base + DMAC_SAR, s->play ? mem : s->d->fifo);
	dmac_wr(au, base + DMAC_DAR, s->play ? s->d->fifo : mem);
	dmac_wr(au, base + DMAC_LLP, s->lli_dma + AUX_ITEM * sizeof(struct lli));
	dmac_wr(au, base + DMAC_CTL_LO, CTL_LLP_EN);	/* first block comes from the list */
	dmac_wr(au, base + DMAC_CTL_HI, 0);
	dmac_wr(au, base + DMAC_CFG_LO, DMA_CFG_LO);
	dmac_wr(au, base + DMAC_CFG_HI, s->d->cfg_hi);
	dmac_chbit(au, DMAC_MASK_BLOCK, ch, 1);
	dmac_chbit(au, DMAC_MASK_ERR, ch, 1);
	dmac_chbit(au, DMAC_CHEN, ch, 1);
}

static void t48s_start(struct t48s_stream *s)
{
	struct t48s_audio *au = s->codec->au;

	s->err_armed = 0;
	s->err_run = 0;
	s->starts++;
	s->running = 1;
	if (!s->play) {
		t48s_dma_go(s);
		return;
	}
	/* the block of zeros now leads into the ring; if the channel is already
	 * going round it, it follows within two blocks */
	s->lli[AUX_ITEM].llp = s->lli_dma;
	wmb();
	if (!s->dma_on || !(dmac_rd(au, DMAC_CHEN) & (1 << s->d->dma_ch))) {
		t48s_dma_go(s);
		s->dma_on = 1;
	}
}

static void t48s_stop(struct t48s_stream *s)
{
	struct t48s_audio *au = s->codec->au;
	struct snd_pcm_runtime *rt = s->ss ? s->ss->runtime : NULL;
	u32 base = DMAC_CH(s->d->dma_ch);
	u32 zeros = s->lli_dma + AUX_ITEM * sizeof(struct lli);
	u32 llp, sar, n, k;
	s16 *fade = (s16 *)((u8 *)s->lli + FADE_OFF);
	int last = 0, i;

	s->running = 0;
	apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) & ~s->d->err_bit);
	if (!s->play) {
		dmac_quiet(au, s->d->dma_ch);
		return;
	}
	/*
	 * Playback is not cut off: the DAC would hold or drop whatever level it
	 * had. The block being played finishes; the two list items that could
	 * follow it are turned into a fade from its last sample to zero, and the
	 * fade leads to the block of zeros, which goes round until the next
	 * start or until the stream is closed.
	 */
	s->lli[AUX_ITEM].llp = zeros;
	s->stopped = jiffies;
	llp = dmac_rd(au, base + DMAC_LLP);
	sar = dmac_rd(au, base + DMAC_SAR);
	if (llp < s->lli_dma || !rt || !rt->dma_area)
		return;
	n = (llp - s->lli_dma) / sizeof(struct lli);	/* the item that comes next */
	if (n >= s->nblk)
		return;		/* already on the zeros */
	if (sar < s->lli_dma + AUX_OFF || sar > s->lli_dma + LLI_BYTES) {
		/* in the ring: block n - 1 is playing */
		const s16 *pcm = (const s16 *)rt->dma_area;

		last = pcm[(n ? n : s->nblk) * BLK_FRAMES - 1];
	}
	for (i = 0; i < BLK_FRAMES; i++)
		fade[i] = last * (BLK_FRAMES - 1 - i) / BLK_FRAMES;
	for (k = 0; k < 2; k++) {
		struct lli *l = &s->lli[(n + k) % s->nblk];

		l->llp = zeros;
		l->sar = s->lli_dma + FADE_OFF;
		l->ctl_lo = TX_CTL_LO;
	}
	wmb();
}

static int t48s_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct t48s_stream *s = t48s_stream_of(ss);
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&s->codec->au->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		t48s_start(s);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		t48s_stop(s);
		break;
	default:
		rc = -EINVAL;
	}
	spin_unlock_irqrestore(&s->codec->au->lock, flags);
	return rc;
}

static snd_pcm_uframes_t t48s_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct t48s_stream *s = t48s_stream_of(ss);
	struct t48s_audio *au = s->codec->au;
	struct snd_pcm_runtime *rt = ss->runtime;
	u32 base = DMAC_CH(s->d->dma_ch);
	u32 bytes = s->nblk * BLK_BYTES;
	u32 aux = s->lli_dma + AUX_OFF;
	u32 llp, mem, blk, start, off;
	int tries = 4;

	/* the LLP register points at the item after the one in hand; the memory
	 * side's address register says how far into that one the DMAC is */
	do {
		llp = dmac_rd(au, base + DMAC_LLP);
		mem = dmac_rd(au, base + (s->play ? DMAC_SAR : DMAC_DAR));
	} while (llp != dmac_rd(au, base + DMAC_LLP) && --tries);
	if (!tries)
		mem = 0;
	blk = (llp - s->lli_dma) / sizeof(struct lli);
	if (llp < s->lli_dma || blk >= s->nblk)
		return 0;
	if (mem >= aux && mem <= s->lli_dma + LLI_BYTES)
		return 0;	/* not in the ring yet: draining the FIFO, or on the zeros */
	blk = (blk + s->nblk - 1) % s->nblk;
	start = blk * BLK_BYTES;
	/* finer than a block when the address agrees (it may already stand at the
	 * start of the next block while the next item is being loaded) */
	off = mem - rt->dma_addr;
	if (mem >= rt->dma_addr && off >= start && off <= start + BLK_BYTES)
		start = off % bytes;
	else if (mem == rt->dma_addr && blk == s->nblk - 1)
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
 * Block interrupt of one DMA0 channel: a period has gone by. The DMAC also
 * raises a few that no period-ending item asked for (measured on capture:
 * about 5 in 1000 alone, 50 in 1000 while the other codec records too, none
 * when every item interrupts). They are harmless: the pointer is exact, so
 * ALSA sees a period that is not complete yet and waits for the real one.
 */
static irqreturn_t t48s_dma_irq(int irq, void *data)
{
	struct t48s_stream *s = data;
	struct t48s_audio *au = s->codec->au;
	int ch = s->d->dma_ch;
	unsigned long flags;
	int running, err;

	spin_lock_irqsave(&au->lock, flags);
	err = dmac_rd(au, DMAC_STATUS_ERR) & (1 << ch);
	dmac_wr(au, DMAC_CLEAR_BLOCK, 1 << ch);
	dmac_wr(au, DMAC_CLEAR_TFR, 1 << ch);
	dmac_wr(au, DMAC_CLEAR_ERR, 1 << ch);
	s->irqs++;
	if (err)
		s->dma_errors++;
	running = s->running;
	if (running && !s->err_armed) {
		/* the ADC FIFO sat full and the DAC FIFO empty before the start;
		 * from now on an overflow or underflow is real */
		s->err_armed = 1;
		apm_wr(au, APM_IRQSTAT, s->d->err_bit);
		apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) | s->d->err_bit);
	}
	spin_unlock_irqrestore(&au->lock, flags);

	if (err)
		pr_warn_ratelimited(DRV_NAME ": %s %s: DMA error\n", s->codec->d->name,
				    s->play ? "playback" : "capture");
	if (running && s->ss)
		snd_pcm_period_elapsed(s->ss);
	return IRQ_HANDLED;
}

/* APM interrupt: only the FIFO error bits of running streams are enabled
 * (ADC overflow, DAC underflow) */
static irqreturn_t t48s_apm_irq(int irq, void *data)
{
	struct t48s_audio *au = data;
	unsigned long flags;
	u32 st;
	int i;

	spin_lock_irqsave(&au->lock, flags);
	st = apm_rd(au, APM_IRQSTAT) & apm_rd(au, APM_IRQEN);
	for (i = 0; i < 4; i++) {
		struct t48s_stream *s = &au->codec[i / 2].st[i % 2];

		if (!(st & s->d->err_bit))
			continue;
		apm_wr(au, APM_IRQSTAT, s->d->err_bit);
		s->fifo_errors++;
		if (++s->err_run >= FIFO_ERR_LIMIT) {
			apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) & ~s->d->err_bit);
			pr_warn(DRV_NAME ": %s: %s, no longer counting\n", s->codec->d->name,
				s->play ? "DAC FIFO keeps running dry" : "ADC FIFO keeps overflowing");
		}
	}
	st &= ~APM_IRQ_FIFO;
	if (st) {	/* nothing else should be enabled; make sure it cannot storm */
		apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) & ~st);
		pr_warn(DRV_NAME ": unexpected APM interrupt 0x%x, disabled\n", st);
	}
	spin_unlock_irqrestore(&au->lock, flags);
	return IRQ_HANDLED;
}

/* ---- mixer controls ---- */

enum { CTL_SWITCH, CTL_SOURCE, CTL_PREAMP, CTL_DGAIN, CTL_BIAS_A, CTL_BIAS_B,
       CTL_OUT_A, CTL_OUT_B, CTL_OUTVOL_A, CTL_OUTVOL_B, CTL_DAC, CTL_ST, CTL_STVOL,
       CTL_AMP };
#define CTL_PV(codec, kind)	(((codec) << 8) | (kind))
#define CTL_CODEC(kc)		(((kc)->private_value >> 8) & 1)
#define CTL_KIND(kc)		((kc)->private_value & 0xff)

/* unity * 10^(db/20), for a gain that is 1.0 at "unity" */
static u32 db_gain(u32 unity, int db)
{
	u32 g = unity << 8;

	for (; db > 0; db--)
		g = (u32)(((u64)g * 73533) >> 16);	/* +1 dB */
	for (; db < 0; db++)
		g = (u32)(((u64)g * 58409) >> 16);	/* -1 dB */
	return (g + 128) >> 8;
}

/* digital gains: step 0 mutes, step n is (n - 36) dB; the register holds a
 * 15-bit gain and a shift, as ApmHW_DigitalGainSet writes them */
static u32 dgain_bits(u32 unity, int shift_pos, int step)
{
	u32 g, shift = 3;

	if (step <= 0)
		return shift << shift_pos;
	g = db_gain(unity, step - DGAIN_0DB_STEP);
	while (g >= 0x8000) {
		g >>= 1;
		shift++;
	}
	return (shift << shift_pos) | g;
}

static int adc_step_of(u32 reg)
{
	u32 want = (reg & 0x7fff) << (((reg >> 24) & 0xf) > 3 ? ((reg >> 24) & 0xf) - 3 : 0);
	u32 best = ~0;
	int step, found = 0;

	if (!(reg & 0xffff))
		return 0;
	for (step = 1; step <= ADC_MAX_STEP; step++) {
		u32 b = dgain_bits(ADC_UNITY, 24, step);
		u32 v = (b & 0x7fff) << ((b >> 24) - 3);
		u32 diff = v > want ? v - want : want - v;

		if (diff < best) {
			best = diff;
			found = step;
		}
	}
	return found;
}

static void t48s_dac_gain(struct t48s_codec *c, int step)
{
	apm_update(c->au, c->d->dac_dgain_reg, ~DAC_DGAIN_KEEP, dgain_bits(DAC_UNITY, 16, step));
	c->dac_step = step;
}

/*
 * Sidetone level, as ApmHW_SidetoneVolumeSet does it: the level is one of 28
 * coefficients of the sidetone filter, written through the coefficient port
 * with the sidetone path switched off meanwhile. The other values are the
 * ones that function writes.
 */
static void t48s_sidetone_gain(struct t48s_codec *c, int step)
{
	struct t48s_audio *au = c->au;
	u32 coef[ST_COEFS] = { 0 };
	u32 g = step > 0 ? db_gain(ST_UNITY, step - DGAIN_0DB_STEP) : 0;
	u32 was = apm_rd(au, c->d->sidetone_reg) & APM_SIDETONE_EN;
	int i;

	coef[0] = ST_UNITY - 1;
	coef[5] = 102;
	coef[6] = min_t(u32, g, ST_UNITY - 1);
	coef[24] = 6;
	coef[25] = ST_UNITY - 1;
	coef[27] = 102;
	if (was)
		apm_update(au, c->d->sidetone_reg, APM_SIDETONE_EN, 0);
	apm_wr(au, APM_COEF_ADDR, 0);
	for (i = 0; i < ST_COEFS; i++) {
		u32 addr = c->d->sidetone_coef | i;

		apm_wr(au, APM_COEF_DATA, coef[i]);
		apm_wr(au, APM_COEF_ADDR, addr);
		apm_wr(au, APM_COEF_ADDR, addr | APM_COEF_STROBE);
		apm_wr(au, APM_COEF_ADDR, addr);
	}
	apm_wr(au, APM_COEF_ADDR, 0);
	if (was)
		apm_update(au, c->d->sidetone_reg, APM_SIDETONE_EN, APM_SIDETONE_EN);
	c->sidetone_step = step;
}

static int t48s_out_is_on(struct t48s_audio *au, int out)
{
	const struct t48s_out_desc *o = &t48s_outs[out];

	return !(apm_rd(au, APM_OUT_MUTE) & o->mute) &&
	       ((apm_rd(au, APM_OUT_MODE) >> o->mode_shift) & 3) != 3;
}

/* May sleep. On: the stage is switched on while still muted, then unmuted.
 * Off: muted (the codec ramps it down), and only then switched off. */
static void t48s_out_switch(struct t48s_audio *au, int out, int on)
{
	const struct t48s_out_desc *o = &t48s_outs[out];

	if (on) {
		apm_update(au, APM_OUT_MODE, 3 << o->mode_shift, 0);
		msleep(20);
		apm_update(au, APM_OUT_MUTE, o->mute, 0);
	} else {
		if (out == OUT_SPEAKER)
			t48s_amp(au, 0);
		apm_update(au, APM_OUT_MUTE, o->mute, o->mute);
		msleep(MUTE_RAMP_MS);
		apm_update(au, APM_OUT_MODE, 3 << o->mode_shift, 3 << o->mode_shift);
	}
}

static int t48s_ctl_max(int kind)
{
	switch (kind) {
	case CTL_PREAMP:
		return PGA_MAX_STEP;
	case CTL_DGAIN:
		return ADC_MAX_STEP;
	case CTL_OUTVOL_A:
	case CTL_OUTVOL_B:
		return OUT_MAX_STEP;
	case CTL_DAC:
		return DAC_MAX_STEP;
	case CTL_STVOL:
		return ST_MAX_STEP;
	default:
		return 1;
	}
}

static int t48s_ctl_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	static const char * const src[2] = { "Input A", "Input B" };
	int max = t48s_ctl_max(CTL_KIND(kc));

	ui->count = 1;
	if (CTL_KIND(kc) == CTL_SOURCE)
		return snd_ctl_enum_info(ui, 1, 2, src);
	ui->type = max > 1 ? SNDRV_CTL_ELEM_TYPE_INTEGER : SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	ui->value.integer.min = 0;
	ui->value.integer.max = max;
	return 0;
}

static int t48s_ctl_read(struct t48s_codec *c, int kind)
{
	struct t48s_audio *au = c->au;
	const struct t48s_codec_desc *d = c->d;
	int codec = c - au->codec;
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
		return c->adc_step;
	case CTL_BIAS_A:
		return !(apm_rd(au, d->micdet_reg) & APM_MICDET_BIASA_OFF);
	case CTL_BIAS_B:
		return !(apm_rd(au, APM_ANA) & d->biasb_off);
	case CTL_OUT_A:
	case CTL_OUT_B:
		return t48s_out_is_on(au, codec * 2 + (kind - CTL_OUT_A));
	case CTL_OUTVOL_A:
	case CTL_OUTVOL_B:
		v = t48s_outs[codec * 2 + (kind - CTL_OUTVOL_A)].gain_shift;
		return OUT_MAX_STEP - ((apm_rd(au, APM_OUT_MODE) >> v) & 7);
	case CTL_DAC:
		return c->dac_step;
	case CTL_ST:
		return !!(apm_rd(au, d->sidetone_reg) & APM_SIDETONE_EN);
	case CTL_STVOL:
		return c->sidetone_step;
	default:
		return !!(readl(au->gpio) & GPIO_SPK_AMP);
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
	int codec = CTL_CODEC(kc);
	int kind = CTL_KIND(kc);
	int v;

	if (kind == CTL_SOURCE)
		v = uc->value.enumerated.item[0];
	else
		v = uc->value.integer.value[0];
	if (v < 0 || v > t48s_ctl_max(kind))
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
		apm_update(au, d->adc_dgain_reg, ~ADC_DGAIN_KEEP, dgain_bits(ADC_UNITY, 24, v));
		c->adc_step = v;
		break;
	case CTL_BIAS_A:
		apm_update(au, d->micdet_reg, APM_MICDET_BIASA_OFF, v ? 0 : APM_MICDET_BIASA_OFF);
		break;
	case CTL_BIAS_B:
		apm_update(au, APM_ANA, d->biasb_off, v ? 0 : d->biasb_off);
		break;
	case CTL_OUT_A:
	case CTL_OUT_B:
		t48s_out_switch(au, codec * 2 + (kind - CTL_OUT_A), v);
		break;
	case CTL_OUTVOL_A:
	case CTL_OUTVOL_B: {
		int sh = t48s_outs[codec * 2 + (kind - CTL_OUTVOL_A)].gain_shift;

		apm_update(au, APM_OUT_MODE, 7 << sh, (OUT_MAX_STEP - v) << sh);
		break;
	}
	case CTL_DAC:
		t48s_dac_gain(c, v);
		break;
	case CTL_ST:
		apm_update(au, d->sidetone_reg, APM_SIDETONE_EN, v ? APM_SIDETONE_EN : 0);
		break;
	case CTL_STVOL:
		t48s_sidetone_gain(c, v);
		break;
	default:
		t48s_amp(au, v);
	}
	return 1;
}

static const DECLARE_TLV_DB_SCALE(t48s_tlv_preamp, 0, 300, 0);
static const DECLARE_TLV_DB_SCALE(t48s_tlv_dgain, -3600, 100, 1);
static const DECLARE_TLV_DB_SCALE(t48s_tlv_out, -2100, 300, 0);

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
#define T48S_CODEC_CTLS(n, codec)						\
	T48S_CTL(n " Capture Switch", codec, CTL_SWITCH),			\
	T48S_CTL(n " Capture Source", codec, CTL_SOURCE),			\
	T48S_VOL(n " Preamp Capture Volume", codec, CTL_PREAMP, t48s_tlv_preamp), \
	T48S_VOL(n " Capture Volume", codec, CTL_DGAIN, t48s_tlv_dgain),	\
	T48S_CTL(n " Mic Bias A Switch", codec, CTL_BIAS_A),			\
	T48S_CTL(n " Mic Bias B Switch", codec, CTL_BIAS_B),			\
	T48S_CTL(n " Output A Playback Switch", codec, CTL_OUT_A),		\
	T48S_CTL(n " Output B Playback Switch", codec, CTL_OUT_B),		\
	T48S_VOL(n " Output A Playback Volume", codec, CTL_OUTVOL_A, t48s_tlv_out), \
	T48S_VOL(n " Output B Playback Volume", codec, CTL_OUTVOL_B, t48s_tlv_out), \
	T48S_VOL(n " Playback Volume", codec, CTL_DAC, t48s_tlv_dgain),	\
	T48S_CTL(n " Sidetone Switch", codec, CTL_ST),				\
	T48S_VOL(n " Sidetone Volume", codec, CTL_STVOL, t48s_tlv_dgain)

static struct snd_kcontrol_new t48s_ctls[] = {
	T48S_CODEC_CTLS("Codec A", 0),
	T48S_CODEC_CTLS("Codec B", 1),
	T48S_CTL("Speaker Amp Switch", 0, CTL_AMP),
};

/* ---- /proc/asound/card0/t48s ---- */

static void t48s_proc_read(struct snd_info_entry *entry, struct snd_info_buffer *buf)
{
	struct t48s_audio *au = entry->private_data;
	static const u16 regs[] = { 0x004, 0x008, 0x00c, 0x020, 0x024, 0x02c, 0x040, 0x044,
				    0x050, 0x054, 0x05c, 0x070, 0x074, 0x120, 0x124, 0x12c,
				    0x134, 0x138, 0x13c, 0x42c, 0x52c };
	int i;

	for (i = 0; i < 4; i++) {
		struct t48s_stream *s = &au->codec[i / 2].st[1 - i % 2];

		snd_iprintf(buf, "%s %s: DMA0 channel %d, %s, starts %lu, period interrupts %lu, "
			    "DMA errors %lu, %s %lu\n",
			    s->codec->d->name, s->play ? "playback" : "capture", s->d->dma_ch,
			    s->running ? "running" : s->dma_on ? "playing zeros" : "stopped",
			    s->starts, s->irqs, s->dma_errors,
			    s->play ? "DAC FIFO underflows" : "ADC FIFO overflows", s->fifo_errors);
	}
	snd_iprintf(buf, "DMA0: enable 0x%x, channels running 0x%02x\n",
		    dmac_rd(au, DMAC_CFG), dmac_rd(au, DMAC_CHEN) & 0xff);
	snd_iprintf(buf, "Speaker amplifier (GPIO31): %s\n",
		    readl(au->gpio) & GPIO_SPK_AMP ? "on" : "off");
	snd_iprintf(buf, "Cold starts by this driver since boot: %d\n", au->cold_starts);
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

/* Take the output side over from whatever ran before: nothing audible, and
 * every level low, until user space asks. May sleep. */
static void t48s_outputs_off(struct t48s_audio *au)
{
	int i;

	t48s_amp(au, 0);
	for (i = 0; i < 2; i++)
		apm_update(au, au->codec[i].d->sidetone_reg, APM_SIDETONE_EN, 0);
	apm_update(au, APM_OUT_MUTE, APM_OUT_MUTE_ALL, APM_OUT_MUTE_ALL);
	msleep(MUTE_RAMP_MS);
	apm_update(au, APM_OUT_MODE, APM_OUT_MODE_ALL | APM_OUT_GAIN_ALL,
		   APM_OUT_MODE_ALL | APM_OUT_GAIN_ALL);
	for (i = 0; i < 2; i++) {
		t48s_dac_gain(&au->codec[i], DAC_START_STEP);
		t48s_sidetone_gain(&au->codec[i], ST_START_STEP);
	}
}

/* ---- cold start ---- */

/*
 * A register of a block that is not clocked or is held in reset does not
 * answer: the bus raises an external abort, a little after the access
 * ("imprecise"), and with nobody catching it that is the end of the kernel
 * (seen on the first start through U-Boot). While t48s_guard is set the abort
 * is noted and execution goes on; at any other time it is the kernel's to
 * report, as before.
 */
static int t48s_guard, t48s_aborted;

static int t48s_abort(unsigned long addr, unsigned int fsr, struct pt_regs *regs)
{
	if (!t48s_guard)
		return 1;
	t48s_aborted++;
	return 0;
}

static void t48s_guard_on(void)
{
	t48s_aborted = 0;
	t48s_guard = 1;
}

/* how many accesses were refused since t48s_guard_on() */
static int t48s_refused(struct t48s_audio *au)
{
	/* an abort from the last access may still be on its way */
	(void)readl(au->chipc);
	udelay(200);
	return t48s_aborted;
}

static int t48s_guard_off(struct t48s_audio *au)
{
	int n = t48s_refused(au);

	t48s_guard = 0;
	return n;
}

/* chip control registers are shared with every other driver: read, change,
 * write with interrupts off (one CPU) */
static void chipc_update(struct t48s_audio *au, u32 off, u32 mask, u32 val)
{
	unsigned long flags;

	local_irq_save(flags);
	writel((readl(au->chipc + off) & ~mask) | (val & mask), au->chipc + off);
	local_irq_restore(flags);
}

/* NULL if chip control has the block switched on, else what is missing. Reads
 * chip control only: the block's own registers must not be touched before
 * this says NULL. */
static const char *t48s_apm_gated(struct t48s_audio *au)
{
	if (!(readl(au->chipc + CHIPC_APM100_CLK) & CHIPC_CLK_OUTPUT) ||
	    !(readl(au->chipc + CHIPC_APM200_CLK) & CHIPC_CLK_OUTPUT))
		return "clocks gated";
	if ((readl(au->chipc + CHIPC_AUDIO_EN) & CHIPC_AUDIO_EN_OURS) != CHIPC_AUDIO_EN_OURS)
		return "audio channels disabled";
	if (readl(au->chipc + CHIPC_SOFTRESET2) & CHIPC_SOFTRESET2_APM)
		return "held in reset";
	if (!(readl(au->chipc + CHIPC_BUSCLK) & CHIPC_BUSCLK_DMAC0))
		return "DMA controller clock gated";
	return NULL;
}

/* the same, and then the block's own state. Call with the guard on if there
 * is any doubt that it answers. */
static const char *t48s_apm_missing(struct t48s_audio *au)
{
	const char *gated = t48s_apm_gated(au);

	if (gated)
		return gated;
	if ((apm_rd(au, APM_CTL) & APM_CTL_CODECS) != APM_CTL_CODECS)
		return "codecs not enabled";
	if (apm_rd(au, APM_RATE_A) != APM_RATE_16K || apm_rd(au, APM_RATE_B) != APM_RATE_16K)
		return "not at 16 kHz";
	return NULL;
}

/*
 * For testing only: put the block where a power-on leaves it. Held in reset
 * (which puts every APM register back to its default: outputs muted, DACs
 * and ADCs powered down), clocks gated, audio channels disabled, the audio
 * DMA controller disabled and its bus clock gated. No stream may be open.
 */
static void t48s_apm_power_off(struct t48s_audio *au)
{
	int i;

	t48s_amp(au, 0);
	apm_update(au, APM_OUT_MUTE, APM_OUT_MUTE_ALL, APM_OUT_MUTE_ALL);
	msleep(MUTE_RAMP_MS);
	for (i = 0; i < 4; i++)
		dmac_quiet(au, au->codec[i / 2].st[i % 2].d->dma_ch);
	dmac_wr(au, DMAC_CFG, 0);
	chipc_update(au, CHIPC_SOFTRESET2, CHIPC_SOFTRESET2_APM, CHIPC_SOFTRESET2_APM);
	chipc_update(au, CHIPC_AUDIO_EN, CHIPC_AUDIO_EN_OURS, 0);
	chipc_update(au, CHIPC_APM100_CLK, CHIPC_CLK_OUTPUT, 0);
	chipc_update(au, CHIPC_APM200_CLK, CHIPC_CLK_OUTPUT, 0);
	chipc_update(au, CHIPC_BUSCLK, CHIPC_BUSCLK_DMAC0, 0);
	msleep(20);
}

/* ApmHw_SampleFreqSet(codec, 16000), as in Yealink's kernel: the rate, then
 * two values loaded by strobes, and the ADC's unity gain for this rate */
static void t48s_rate_16k(struct t48s_audio *au, int codec)
{
	const struct t48s_codec_desc *d = &t48s_codecs[codec];
	u32 gain_strobe = codec ? 0x10 : 0x08, strobe = codec ? 0x04 : 0x02;
	u32 asrc_reg = codec ? 0x058 : 0x028;

	apm_wr(au, d->rate_reg, APM_RATE_16K);
	apm_wr(au, APM_RATE_LOAD, APM_RATE_LOAD_16K);
	apm_update(au, APM_RATE_CTL, gain_strobe, gain_strobe);
	apm_update(au, d->adc_dgain_reg, ~ADC_DGAIN_KEEP, dgain_bits(ADC_UNITY, 24, DGAIN_0DB_STEP));
	apm_update(au, APM_RATE_CTL, gain_strobe, 0);
	apm_wr(au, APM_RATE_LOAD, APM_RATE_LOAD_IDLE);
	apm_update(au, APM_RATE_CTL, strobe, strobe);
	apm_update(au, asrc_reg, 0x18000000, 0);	/* both clear at 43 kHz and below */
	apm_update(au, d->adc_dgain_reg, 0x60000000, 0);
	apm_update(au, APM_RATE_CTL, strobe, 0);
}

/*
 * Bring the APM block up from the state a power-on leaves. The order is
 * Broadcom's (apmInit, apmPrepare and apmEnable in their GPL halaudio APM
 * driver); the register writes are those of ApmHw_init, ApmHw_SampleFreqSet
 * and ApmHw_ChEnable in Yealink's kernel image. Ends with the analog side as
 * Yealink's firmware idles (so both ways of starting give the same thing),
 * except that every output is muted, switched off and at its lowest gain.
 * May sleep. Call with the guard on.
 */
#define APM_UDELAY	0xffff
static const struct { u16 reg; u32 val; } t48s_apm_init[] = {
	{ APM_CTL, APM_CTL_INIT }, { 0x20c, 0x4 },
	/* codec A */
	{ APM_CODEC_A, 0xe0000000 }, { APM_UDELAY }, { 0x018, 0x420 },
	{ APM_CODEC_A, 0xe0880000 }, { APM_SIDETONE_A, 0 }, { 0x028, 0x02130000 },
	{ APM_DAC_DGAIN_A, 0x00337fff }, { 0x03c, 0x31d }, { APM_ADC_DGAIN_A, 0x031314f8 },
	{ APM_RATE_A, 0x20000000 }, { 0x048, 0x10 }, { 0x04c, 0x5000 },
	/* codec B */
	{ APM_CODEC_B, 0x80000000 }, { APM_UDELAY }, { 0x01c, 0x420 },
	{ APM_CODEC_B, 0x80880000 }, { APM_SIDETONE_B, 0 }, { 0x058, 0x02130000 },
	{ APM_DAC_DGAIN_B, 0x00337fff }, { 0x06c, 0x31d }, { APM_ADC_DGAIN_B, 0x031314f8 },
	{ APM_RATE_B, 0x20000000 }, { 0x078, 0x10 }, { 0x07c, 0x5000 },
	/* codec C (an ADC nothing is wired to; set up as Broadcom does, never enabled) */
	{ 0x21c, 0x80000000 }, { APM_UDELAY }, { 0x218, 0x400 }, { 0x21c, 0x80080000 },
	{ 0x284, 0 }, { 0x220, 0x205 }, { 0x224, 0x031314f8 }, { 0x228, 0x20000000 },
	{ 0x22c, 0x10 },
	/* shared */
	{ 0x0a0, 0x80000000 }, { APM_RATE_LOAD, APM_RATE_LOAD_IDLE }, { APM_RATE_CTL, 0x6 },
	{ 0x230, 0x2 }, { 0x234, 0 }, { 0x088, 0 }, { 0x168, 0 }, { 0x164, 0 },
	{ 0x160, 0x80000000 },
};

static void t48s_apm_cold_start(struct t48s_audio *au)
{
	static const u16 fifo_regs[] = { 0x428, 0x528, 0x420, 0x520 };
	unsigned long flags;
	int i;

	/* the loudspeaker amplifier's pin first: plain GPIO, low, output */
	chipc_update(au, CHIPC_GPIOMUX3, 0xf0000000, 0);
	t48s_amp(au, 0);
	local_irq_save(flags);
	writel(readl(au->gpio + GPIO_DIR) | GPIO_SPK_AMP, au->gpio + GPIO_DIR);
	local_irq_restore(flags);

	/* clocks, audio channels, and a reset so that every register starts
	 * from its default */
	chipc_update(au, CHIPC_BUSCLK, CHIPC_BUSCLK_DMAC0, CHIPC_BUSCLK_DMAC0);
	writel(CHIPC_APM200_CLK_ON, au->chipc + CHIPC_APM200_CLK);
	writel(CHIPC_APM100_CLK_ON, au->chipc + CHIPC_APM100_CLK);
	chipc_update(au, CHIPC_AUDIO_EN, CHIPC_AUDIO_EN_CLOCKS, CHIPC_AUDIO_EN_CLOCKS);
	chipc_update(au, CHIPC_AUDIO_EN, CHIPC_AUDIO_EN_CODECS, CHIPC_AUDIO_EN_CODECS);
	chipc_update(au, CHIPC_SOFTRESET2, CHIPC_SOFTRESET2_APM, CHIPC_SOFTRESET2_APM);
	udelay(100);
	chipc_update(au, CHIPC_SOFTRESET2, CHIPC_SOFTRESET2_APM, 0);
	udelay(50);

	/* the first access to the block: if it is refused, leave it alone */
	(void)apm_rd(au, APM_CTL);
	if (t48s_refused(au))
		return;

	/* ApmHw_init */
	apm_wr(au, APM_IRQSTAT, 0xff);
	apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) & ~0xff);
	for (i = 0; i < ARRAY_SIZE(t48s_apm_init); i++) {
		if (t48s_apm_init[i].reg == APM_UDELAY)
			udelay(50);
		else
			apm_wr(au, t48s_apm_init[i].reg, t48s_apm_init[i].val);
	}
	apm_update(au, 0x0a0, 0x80000000, 0);
	apm_update(au, APM_DAC, 0x180, 0);
	apm_update(au, APM_ANA, 1, 0);
	apm_update(au, 0x434, 1, 1);
	apm_update(au, 0x534, 1, 1);
	apm_wr(au, 0x438, 4);
	apm_wr(au, 0x538, 4);
	apm_update(au, APM_MICDET_A, 1, 1);
	apm_update(au, APM_MICDET_B, 1, 1);
	for (i = 0; i < ARRAY_SIZE(fifo_regs); i++)
		apm_update(au, fifo_regs[i], 7, 4);	/* FIFO thresholds */
	apm_update(au, APM_RATE_CTL, 0xf00, 0x700);

	/* the output side before anything runs: a reset leaves the four outputs
	 * muted but switched on at 0 dB, with the DACs powered down. Mute ramps
	 * as Yealink has them, then off and at the lowest gain. */
	apm_wr(au, APM_OUT_MUTE, APM_OUT_MUTE_ALL | APM_OUT_RAMPS);
	apm_update(au, APM_DAC, APM_DAC_RAMP_COSINE, APM_DAC_RAMP_COSINE);
	apm_wr(au, APM_OUT_MODE, APM_OUT_MODE_ALL | APM_OUT_GAIN_ALL);

	t48s_rate_16k(au, 0);
	t48s_rate_16k(au, 1);

	/* ApmHw_ChEnable for codecs A and B */
	apm_wr(au, APM_IRQSTAT, APM_IRQ_FIFO);
	apm_wr(au, APM_FIFO_CLR_A, ~0);
	apm_wr(au, APM_FIFO_CLR_B, ~0);
	apm_wr(au, APM_FIFO_CLR, ~0);
	apm_wr(au, APM_FLUSH, 0xf);
	apm_update(au, APM_CTL, APM_CTL_CODECS, APM_CTL_CODECS);
	udelay(25);
	apm_update(au, APM_CODEC_A, APM_CODEC_HOLD_AB, 0);

	/* the analog side as Yealink idles: microphone bias supplies on, codec B's
	 * ADC on, on input B (the handset microphone) at 15 dB and 0 dB digital;
	 * codec A's ADC off and its digital gain muted */
	apm_wr(au, APM_ANA, APM_ANA_ADC_B_ON);
	apm_wr(au, APM_AFE_A, 0);
	apm_wr(au, APM_AFE_B, APM_AFE_INPUT_B | (15 << APM_AFE_PGA_SHIFT));
	apm_update(au, APM_MICDET_A, APM_MICDET_BIASA_OFF, 0);
	apm_update(au, APM_MICDET_B, APM_MICDET_BIASA_OFF, 0);
	apm_update(au, APM_ADC_DGAIN_A, ~ADC_DGAIN_KEEP, dgain_bits(ADC_UNITY, 24, 0));

	/* last, with every output stage off and muted: power the DACs */
	apm_update(au, APM_DAC, APM_DAC_POWERDOWN, 0);
	msleep(20);
	au->cold_starts++;
}

/* The block is running, by Yealink's hand or ours. Enable no interrupt until
 * a stream runs (Yealink leaves the four FIFO error interrupts enabled; with
 * no DMA running they are all pending), take the outputs over, and learn the
 * gains that are not kept in a readable form. May sleep. */
static void t48s_take_over(struct t48s_audio *au)
{
	int i;

	apm_wr(au, APM_IRQEN, apm_rd(au, APM_IRQEN) & ~0xff);
	t48s_outputs_off(au);
	for (i = 0; i < 2; i++)
		au->codec[i].adc_step = adc_step_of(apm_rd(au, au->codec[i].d->adc_dgain_reg));
}

/*
 * Make sure the block runs, starting it if it does not. Returns NULL, or why
 * there is no audio hardware to use. Never lets a refused access stop the
 * kernel. May sleep.
 */
static const char *t48s_bring_up(struct t48s_audio *au, const char *why)
{
	const char *missing;

	t48s_guard_on();
	if (!why) {
		why = t48s_apm_missing(au);
		if (t48s_refused(au)) {
			/* chip control says on, the block says nothing: start it too */
			why = "it does not answer";
			t48s_aborted = 0;
		}
	}
	if (why) {
		pr_info(DRV_NAME ": audio block is off (%s): starting it\n", why);
		t48s_apm_cold_start(au);
	}
	missing = t48s_refused(au) ? NULL : t48s_apm_missing(au);
	if (!missing && !t48s_refused(au))
		t48s_take_over(au);
	if (t48s_guard_off(au))
		return "it does not answer";
	if (missing)
		return missing;
	if (why)
		pr_info(DRV_NAME ": audio block started from cold\n");
	return NULL;
}

/*
 * /sys/module/t48s_audio/parameters/cold: a test of the cold start with no
 * cold boot. Writing 1 puts the block into its power-on state, shows that a
 * read of it is refused and caught, and starts it again; it fails with "busy"
 * while any stream is open. Stop the audio service first: mixer settings are
 * back at the driver's defaults afterwards, and nothing may touch the mixer
 * meanwhile. Reading gives how often this driver has cold-started the block
 * since boot.
 */
static struct t48s_audio *the_audio;

static int cold_set(const char *val, const struct kernel_param *kp)
{
	struct t48s_audio *au = the_audio;
	const char *missing;
	unsigned long flags;
	int i, v, busy = 0, refused;

	if (kstrtoint(val, 0, &v) || v != 1)
		return -EINVAL;
	if (!au)
		return -ENODEV;
	spin_lock_irqsave(&au->lock, flags);
	for (i = 0; i < 4; i++)
		if (au->codec[i / 2].st[i % 2].ss)
			busy = 1;
	spin_unlock_irqrestore(&au->lock, flags);
	if (busy)
		return -EBUSY;
	t48s_apm_power_off(au);
	pr_info(DRV_NAME ": test: audio block switched off (%s)\n", t48s_apm_gated(au));
	t48s_guard_on();
	(void)apm_rd(au, APM_IRQEN);
	refused = t48s_guard_off(au);
	pr_info(DRV_NAME ": test: a read of the block while off was %s\n",
		refused ? "refused, and caught" : "answered");
	missing = t48s_bring_up(au, "switched off for the test");
	if (missing) {
		pr_err(DRV_NAME ": test: cold start failed: %s\n", missing);
		return -EIO;
	}
	return 0;
}

static int cold_get(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%d\n", the_audio ? the_audio->cold_starts : 0);
}

static const struct kernel_param_ops cold_ops = { .set = cold_set, .get = cold_get };
module_param_cb(cold, &cold_ops, NULL, 0644);

static int __init t48s_audio_init(void)
{
	struct platform_device *pdev;
	struct snd_card *card;
	struct t48s_audio *au;
	const char *missing;
	int i, j, rc;

	/* fault 22, the imprecise external abort: what a register of a block
	 * that is switched off answers with */
	hook_fault_code(22, t48s_abort, SIGBUS, BUS_OBJERR, "imprecise external abort");
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
	strlcpy(card->longname, "Yealink T48S audio (BCM11188 APM codecs A and B)",
		sizeof(card->longname));
	strlcpy(card->mixername, "BCM11188 APM", sizeof(card->mixername));

	rc = -ENOMEM;
	au->apm = ioremap(APM_PHYS, APM_SIZE);
	au->dmac = ioremap(DMA0_PHYS, DMA0_SIZE);
	au->gpio = ioremap(GPIO0_PHYS, GPIO0_SIZE);
	au->chipc = ioremap(CHIPC_PHYS, CHIPC_SIZE);
	if (!au->apm || !au->dmac || !au->gpio || !au->chipc)
		goto fail_card;

	t48s_halt_dsp();

	for (i = 0; i < 2; i++) {
		struct t48s_codec *c = &au->codec[i];

		c->d = &t48s_codecs[i];
		c->au = au;
		for (j = 0; j < 2; j++) {
			c->st[j].d = &c->d->dir[j];
			c->st[j].codec = c;
			c->st[j].play = j == SNDRV_PCM_STREAM_PLAYBACK;
		}
	}
	/* one line for the log of a cold start: what the boot loader left */
	pr_info(DRV_NAME ": chip control as found: APM clocks 0x%08x 0x%08x, audio enable 0x%02x, reset 0x%08x, bus clocks 0x%08x, PLL2 0x%08x\n",
		readl(au->chipc + CHIPC_APM100_CLK), readl(au->chipc + CHIPC_APM200_CLK),
		readl(au->chipc + CHIPC_AUDIO_EN), readl(au->chipc + CHIPC_SOFTRESET2),
		readl(au->chipc + CHIPC_BUSCLK), readl(au->chipc + CHIPC_PLL2_PREDIV));
	missing = t48s_bring_up(au, NULL);
	if (missing) {
		pr_err(DRV_NAME ": no sound: the audio block could not be started (%s)\n", missing);
		rc = -ENODEV;
		goto fail_quiet;
	}
	if (au->cold_starts && readl(au->chipc + CHIPC_PLL2_PREDIV) != CHIPC_PLL2_PREDIV_OK)
		pr_warn(DRV_NAME ": PLL2 is not set as expected: the sample rate may be off\n");

	rc = request_irq(IRQ_APM, t48s_apm_irq, 0, "t48s_audio apm", au);
	if (rc)
		goto fail_card;

	for (i = 0; i < 2; i++) {
		static const char * const irqname[2][2] = {
			{ "t48s_audio A out", "t48s_audio A in" },
			{ "t48s_audio B out", "t48s_audio B in" },
		};
		struct t48s_codec *c = &au->codec[i];
		struct snd_pcm *pcm;

		for (j = 0; j < 2; j++) {
			struct t48s_stream *s = &c->st[j];

			rc = -ENOMEM;
			s->lli = dma_alloc_coherent(&pdev->dev, LLI_BYTES, &s->lli_dma, GFP_KERNEL);
			if (!s->lli)
				goto fail_card;
			memset(s->lli, 0, LLI_BYTES);
			dmac_quiet(au, s->d->dma_ch);
			rc = request_irq(s->d->irq, t48s_dma_irq, 0, irqname[i][j], s);
			if (rc)
				goto fail_card;
		}

		rc = snd_pcm_new(card, i ? "T48S codec B" : "T48S codec A", i, 1, 1, &pcm);
		if (rc < 0)
			goto fail_card;
		pcm->private_data = c;
		strlcpy(pcm->name, i ? "T48S codec B" : "T48S codec A", sizeof(pcm->name));
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &t48s_pcm_ops);
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &t48s_pcm_ops);
		snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV, &pdev->dev,
						      MAX_BLKS * BLK_BYTES, MAX_BLKS * BLK_BYTES);
	}

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
	the_audio = au;
	pr_info(DRV_NAME ": codec A (hw:0,0) and codec B (hw:0,1), capture and playback, 16 kHz mono; outputs off\n");
	return 0;

fail_card:
	pr_err(DRV_NAME ": init failed (%d)\n", rc);
fail_quiet:
	return rc;	/* this is built in: the few allocations are left behind */
fail_pdev:
	platform_device_unregister(pdev);
	return rc;
}
late_initcall(t48s_audio_init);
MODULE_LICENSE("GPL");
