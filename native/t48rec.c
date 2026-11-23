// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48rec: record from an ALSA capture device with the kernel's own ioctls (no
 * alsa-lib), write a WAV file and report what arrived. For testing
 * kernel/drivers/t48s_audio.c on the phone, where there is no arecord.
 *
 *   t48rec [-c CARD] [-d DEVICE] [-s SECONDS] [-p PERIOD] [-n PERIODS] [-o FILE.wav]
 *     -c  card number (default 0)
 *     -d  device: 0 = codec A, 1 = codec B (default 1)
 *     -s  seconds to record (default 5)
 *     -p  period in frames (default 320 = 20 ms)
 *     -n  periods in the buffer (default 8)
 *
 * Always S16_LE, mono, 16000 Hz, which is all the driver offers. The rate is
 * measured against the monotonic clock between the first and the last read.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

#define RATE 16000

static void set_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
	struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	memset(m, 0, sizeof(*m));
	m->bits[bit / 32] = 1u << (bit % 32);
}

static void set_int(struct snd_pcm_hw_params *p, int n, unsigned v)
{
	struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
	memset(i, 0, sizeof(*i));
	i->min = i->max = v;
	i->integer = 1;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void wav_header(FILE *f, uint32_t bytes)
{
	uint32_t h[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16,
			   0x00010001, RATE, RATE * 2, 0x00100002, 0x61746164, bytes };
	fwrite(h, 4, 11, f);	/* the phone and any PC we use are little-endian */
}

int main(int argc, char **argv)
{
	int o, card = 0, dev = 1, secs = 5, period = 320, periods = 8;
	const char *out = 0;
	char path[64];

	while ((o = getopt(argc, argv, "c:d:s:p:n:o:")) != -1) {
		if (o == 'c') card = atoi(optarg);
		else if (o == 'd') dev = atoi(optarg);
		else if (o == 's') secs = atoi(optarg);
		else if (o == 'p') period = atoi(optarg);
		else if (o == 'n') periods = atoi(optarg);
		else if (o == 'o') out = optarg;
		else { fprintf(stderr, "usage: t48rec [-c CARD] [-d DEV] [-s SEC] [-p PERIOD] [-n PERIODS] [-o FILE.wav]\n"); return 2; }
	}
	snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%dc", card, dev);
	int fd = open(path, O_RDONLY);
	if (fd < 0) { perror(path); return 1; }

	struct snd_pcm_hw_params hp;
	memset(&hp, 0, sizeof(hp));
	for (o = 0; o <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; o++)
		memset(&hp.masks[o], 0xff, sizeof(hp.masks[o]));
	for (o = 0; o <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; o++)
		hp.intervals[o].max = ~0u;
	hp.rmask = ~0u;
	set_mask(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	set_mask(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	set_int(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 1);
	set_int(&hp, SNDRV_PCM_HW_PARAM_RATE, RATE);
	set_int(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);
	set_int(&hp, SNDRV_PCM_HW_PARAM_PERIODS, periods);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) { perror("HW_PARAMS (S16_LE mono 16000)"); return 1; }
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { perror("PREPARE"); return 1; }

	unsigned total = secs * RATE, got = 0, overruns = 0, first = 0;
	int16_t *pcm = malloc(total * 2);
	double t_start = now(), t_first = 0, t_last = 0;
	if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0) { perror("START"); return 1; }
	while (got < total) {
		unsigned want = total - got < (unsigned)period ? total - got : (unsigned)period;
		int n = read(fd, pcm + got, want * 2);
		if (n < 0 && errno == EPIPE) {	/* overrun: start again */
			overruns++;
			if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0 || ioctl(fd, SNDRV_PCM_IOCTL_START) < 0) { perror("restart"); break; }
			continue;
		}
		if (n <= 0) { perror("read"); break; }
		t_last = now();
		if (!first) { first = n / 2; t_first = t_last; }
		got += n / 2;
	}
	close(fd);

	printf("%s: %u frames in %.3f s from start", path, got, t_last - t_start);
	if (got > first)
		printf("; %.1f frames/s between first and last read", (got - first) / (t_last - t_first));
	printf("; overruns %u\n", overruns);
	if (!got)
		return 1;

	double sum = 0, sq = 0;
	int mn = 32767, mx = -32768, zeros = 0, step = 0;
	unsigned i;
	for (i = 0; i < got; i++) {
		int v = pcm[i];
		sum += v; sq += (double)v * v;
		if (v < mn) mn = v;
		if (v > mx) mx = v;
		if (v == 0) zeros++;
		if (i && abs(v - pcm[i - 1]) > step) step = abs(v - pcm[i - 1]);
	}
	double mean = sum / got, rms = sqrt(sq / got - mean * mean);
	printf("min %d max %d mean %.1f rms %.1f (%.1f dBFS) zeros %d biggest step %d\n",
	       mn, mx, mean, rms, rms > 0 ? 20 * log10(rms / 32768.0) : -99.0, zeros, step);
	if (out) {
		FILE *f = fopen(out, "wb");
		if (!f) { perror(out); return 1; }
		wav_header(f, got * 2);
		fwrite(pcm, 2, got, f);
		fclose(f);
	}
	return 0;
}
