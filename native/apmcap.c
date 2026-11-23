// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * apmcap: capture microphone samples from the BCM11188 APM block with no DSP
 * and no vendor module, from user space under our own kernel (issue #6).
 *
 * It only records. It never writes a DAC FIFO, a gain, a mux, an amplifier
 * power bit or a GPIO, so it cannot make a sound. It relies on the APM block
 * being left configured by Yealink's kernel (true after a kexec boot).
 *
 *   apmcap [-c 0|1] [-s SECONDS] [-o FILE] [-H] [-r]
 *     -c  APM codec channel (0 = codec A, 1 = codec B; default 1)
 *     -s  seconds to record (default 2)
 *     -o  write raw signed 16-bit little-endian mono samples, 16 kHz
 *     -H  halt the voice DSP first (the write Yealink's vpmHw_Halt does)
 *     -r  only print the APM, DMA and GPIO state, capture nothing
 *
 * How: the ADC FIFO of each codec is a DMA source on controller 0 (the
 * Synopsys DW DMAC at 0x30200000). We run the same channel Yealink used, with
 * the same control words but interrupts off, over a ring of blocks, and poll
 * the write position. The ring lives in RAM our kernel does not own: the 4 MB
 * below the kernel's RAM base that Yealink reserves for the DSP (we use the
 * top of it, 0x00380000, which the DSP image does not reach).
 */
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define APM_BASE   0x30600000u
#define DMA0_BASE  0x30200000u
#define CHIPC_BASE 0x80000000u
#define GPIO_BASE  0x80004000u
#define SCRATCH    0x00380000u	/* physical, outside the kernel's RAM */
#define SCRATCH_SZ 0x00010000u

#define BLK_WORDS  40		/* 160 bytes = 80 samples = 5 ms at 16 kHz */
#define NBLK       64		/* ring of 320 ms */
#define RING_BYTES (NBLK * BLK_WORDS * 4)
#define LLI_OFF    0x8000	/* descriptors after the sample ring */

/* DW DMAC */
#define CH(c, r)   (((c) * 0x58 + (r)) / 4)
#define R_SAR 0x00
#define R_DAR 0x08
#define R_LLP 0x10
#define R_CTL_LO 0x18
#define R_CTL_HI 0x1c
#define R_CFG_LO 0x40
#define R_CFG_HI 0x44
#define R_MASK_TFR 0x310	/* then Block, SrcTran, DstTran, Err, 8 apart */
#define R_DMACFG 0x398
#define R_CHEN   0x3a0

/* Values read from Yealink's running channels (docs/audio.md), INT_EN cleared */
#define CTL_LO_RX 0x1a204826u

struct chan { unsigned dma_ch, fifo, cfg_hi; const char *name; };
static const struct chan chans[2] = {
	{ 3, 0x30670000u, 0x106, "codec A ADC (apm_a_rx)" },
	{ 5, 0x30690000u, 0x206, "codec B ADC (apm_b_rx)" },
};

static int memfd;
static volatile uint32_t *map(uint32_t phys, uint32_t len)
{
	void *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, phys);
	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return p;
}

static void msleep(int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&t, 0);
}

int main(int argc, char **argv)
{
	int c, ch = 1, secs = 2, halt = 0, regs_only = 0;
	const char *out = 0;

	while ((c = getopt(argc, argv, "c:s:o:Hr")) != -1) {
		if (c == 'c') ch = atoi(optarg) ? 1 : 0;
		else if (c == 's') secs = atoi(optarg);
		else if (c == 'o') out = optarg;
		else if (c == 'H') halt = 1;
		else if (c == 'r') regs_only = 1;
		else { fprintf(stderr, "usage: apmcap [-c 0|1] [-s SEC] [-o FILE] [-H] [-r]\n"); return 2; }
	}
	memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd < 0) { perror("/dev/mem"); return 1; }

	volatile uint32_t *apm = map(APM_BASE, 0x1000);
	volatile uint32_t *dma = map(DMA0_BASE, 0x1000);
	volatile uint32_t *chipc = map(CHIPC_BASE, 0x4000);
	volatile uint32_t *gpio = map(GPIO_BASE, 0x1000);
	const struct chan *k = &chans[ch];

	printf("GPIO bank0 data %08x dir %08x: GPIO31 (speaker amp) is %s, level %d\n",
	       gpio[0], gpio[0x400 / 4], (gpio[0x400 / 4] >> 31) ? "output" : "input", gpio[0] >> 31);
	printf("APM ctl[0c]=%08x irqstat[04]=%08x irqen[08]=%08x chA[20]=%08x chB[50]=%08x\n",
	       apm[3], apm[1], apm[2], apm[0x20 / 4], apm[0x50 / 4]);
	printf("APM analog [120]=%08x [134]=%08x [138]=%08x [13c]=%08x [278]=%08x [27c]=%08x\n",
	       apm[0x120 / 4], apm[0x134 / 4], apm[0x138 / 4], apm[0x13c / 4], apm[0x278 / 4], apm[0x27c / 4]);
	printf("DMA0 cfg=%x chen=%02x; DSP: chipc[10]=%08x vpm halt[301c]=%x\n",
	       dma[R_DMACFG / 4], dma[R_CHEN / 4], chipc[0x10 / 4], chipc[0x301c / 4]);
	if (regs_only)
		return 0;

	if (halt) {	/* what vpmHw_Halt does in Yealink's kernel */
		chipc[0x301c / 4] = 1;
		chipc[0x10 / 4] &= ~0x10000u;
		printf("DSP halted: chipc[10]=%08x vpm halt[301c]=%x\n", chipc[0x10 / 4], chipc[0x301c / 4]);
	}

	volatile uint32_t *scr = map(SCRATCH, SCRATCH_SZ);
	volatile uint32_t *lli = scr + LLI_OFF / 4;
	unsigned n = k->dma_ch, i;

	/* stop the channel, mask its interrupts (write-enable bit set, mask bit clear) */
	dma[R_CHEN / 4] = 1u << (n + 8);
	for (i = 0; i < 1000 && (dma[R_CHEN / 4] & (1u << n)); i++)
		;
	for (i = 0; i < 5; i++)
		dma[(R_MASK_TFR + 8 * i) / 4] = 1u << (n + 8);

	memset((void *)scr, 0x55, RING_BYTES);	/* 0x5555 pattern shows unwritten samples */
	for (i = 0; i < NBLK; i++) {
		volatile uint32_t *d = lli + i * 8;
		d[0] = k->fifo;
		d[1] = SCRATCH + i * BLK_WORDS * 4;
		d[2] = SCRATCH + LLI_OFF + ((i + 1) % NBLK) * 32;
		d[3] = CTL_LO_RX;
		d[4] = BLK_WORDS;
		d[5] = d[6] = 0;
	}
	if (dma[R_DMACFG / 4] != 1)
		dma[R_DMACFG / 4] = 1;
	dma[CH(n, R_SAR)] = k->fifo;
	dma[CH(n, R_DAR)] = SCRATCH;
	dma[CH(n, R_LLP)] = SCRATCH + LLI_OFF;
	dma[CH(n, R_CTL_LO)] = 0x18000000;	/* take the first block from the list */
	dma[CH(n, R_CTL_HI)] = 0;
	dma[CH(n, R_CFG_LO)] = 0xc0;
	dma[CH(n, R_CFG_HI)] = k->cfg_hi;
	dma[R_CHEN / 4] = (1u << (n + 8)) | (1u << n);

	unsigned total = secs * 16000, got = 0, rd = 0;
	int16_t *pcm = malloc(total * 2);
	int idle = 0;
	while (got < total && idle < 100) {
		msleep(20);
		/* the descriptor being filled is the one before LLP's target */
		unsigned next = (dma[CH(n, R_LLP)] - (SCRATCH + LLI_OFF)) / 32;
		unsigned wr = ((next + NBLK - 1) % NBLK) * BLK_WORDS * 4;	/* start of the block in progress */
		if (next >= NBLK) { fprintf(stderr, "bad LLP %08x\n", dma[CH(n, R_LLP)]); break; }
		if (wr == rd) { idle++; continue; }
		idle = 0;
		while (rd != wr && got < total) {
			pcm[got++] = *(volatile int16_t *)((volatile char *)scr + rd);
			rd = (rd + 2) % RING_BYTES;
		}
	}
	dma[R_CHEN / 4] = 1u << (n + 8);	/* stop */
	for (i = 0; i < 1000 && (dma[R_CHEN / 4] & (1u << n)); i++)
		;

	printf("%s: %u samples in %d s asked; APM irqstat[04]=%08x\n", k->name, got, secs, apm[1]);
	if (got) {
		double sum = 0, sq = 0;
		int mn = 32767, mx = -32768, zeros = 0, unwritten = 0;
		for (i = 0; i < got; i++) {
			int v = pcm[i];
			sum += v; sq += (double)v * v;
			if (v < mn) mn = v;
			if (v > mx) mx = v;
			if (v == 0) zeros++;
			if (v == 0x5555) unwritten++;
		}
		double mean = sum / got, rms = sqrt(sq / got - mean * mean);
		printf("min %d max %d mean %.1f rms %.1f (%.1f dBFS) zeros %d unwritten %d\n",
		       mn, mx, mean, rms, rms > 0 ? 20 * log10(rms / 32768.0) : -99.0, zeros, unwritten);
		printf("first samples:");
		for (i = 0; i < 16 && i < got; i++)
			printf(" %d", pcm[i]);
		printf("\n");
		if (out) {
			FILE *f = fopen(out, "wb");
			if (!f) { perror(out); return 1; }
			fwrite(pcm, 2, got, f);
			fclose(f);
		}
	}
	return got ? 0 : 1;
}
