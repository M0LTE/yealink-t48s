// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48play: play a test tone or a WAV file to an ALSA playback device with the
 * kernel's own ioctls (no alsa-lib). For testing kernel/drivers/t48s_audio.c
 * on the phone, where there is no aplay.
 *
 *   t48play [-c CARD] [-d DEVICE] [-s SECONDS] [-f HZ] [-l DBFS] [-r MS] [-w FILE.wav]
 *     -c  card number (default 0)
 *     -d  device: 0 = codec A (loudspeaker), 1 = codec B (handset, headset) (default 1)
 *     -s  seconds of tone (default 2)
 *     -f  tone frequency (default 1000)
 *     -l  tone level in dB below full scale, as a negative number (default -30;
 *         nothing above -6 is accepted)
 *     -r  fade the tone in and out over this many ms (default 0: it starts and
 *         stops abruptly, which is the test of the driver's own edges)
 *     -w  play this WAV file instead (16-bit mono 16000 Hz only), as it is
 *
 * It only plays: the outputs, gains and the loudspeaker amplifier are mixer
 * controls (t48mix). Always S16_LE, mono, 16000 Hz. Reports the measured rate
 * and any underruns.
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
#define PERIOD 320
#define PERIODS 4

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

int main(int argc, char **argv)
{
	int o, card = 0, dev = 1, ramp = 0;
	double secs = 2, hz = 1000, db = -30;
	const char *wav = 0;
	char path[64];
	int16_t *pcm;
	unsigned total, i, done = 0, underruns = 0;

	while ((o = getopt(argc, argv, "c:d:s:f:l:r:w:")) != -1) {
		if (o == 'c') card = atoi(optarg);
		else if (o == 'd') dev = atoi(optarg);
		else if (o == 's') secs = atof(optarg);
		else if (o == 'f') hz = atof(optarg);
		else if (o == 'l') db = atof(optarg);
		else if (o == 'r') ramp = atoi(optarg);
		else if (o == 'w') wav = optarg;
		else { fprintf(stderr, "usage: t48play [-c CARD] [-d DEV] [-s SEC] [-f HZ] [-l DBFS] [-r MS] [-w FILE.wav]\n"); return 2; }
	}
	if (wav) {
		uint8_t h[44];
		FILE *f = fopen(wav, "rb");
		long len;

		if (!f) { perror(wav); return 1; }
		if (fread(h, 1, 44, f) != 44 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVEfmt ", 8) ||
		    h[22] != 1 || h[23] || (h[24] | h[25] << 8) != RATE || h[34] != 16 || memcmp(h + 36, "data", 4)) {
			fprintf(stderr, "%s: not a plain 16-bit mono 16000 Hz WAV file\n", wav);
			return 1;
		}
		fseek(f, 0, SEEK_END);
		len = ftell(f) - 44;
		fseek(f, 44, SEEK_SET);
		total = len / 2;
		pcm = malloc(total * 2 + 2);
		if (fread(pcm, 2, total, f) != total) { perror(wav); return 1; }
		fclose(f);
	} else {
		double amp;

		if (db > -6) { fprintf(stderr, "t48play: level %.0f dBFS refused: -6 at most\n", db); return 2; }
		if (secs <= 0 || secs > 60) { fprintf(stderr, "t48play: 0 to 60 seconds\n"); return 2; }
		amp = 32767 * pow(10, db / 20) * sqrt(2);	/* level is rms, like t48rec's */
		total = (unsigned)(secs * RATE);
		pcm = malloc(total * 2 + 2);
		for (i = 0; i < total; i++) {
			double g = 1;
			unsigned edge = i < total - 1 - i ? i : total - 1 - i;
			if (ramp > 0 && edge < (unsigned)ramp * RATE / 1000)
				g = 0.5 - 0.5 * cos(M_PI * edge / (ramp * RATE / 1000.0));
			pcm[i] = (int16_t)lrint(g * amp * sin(2 * M_PI * hz * i / RATE));
		}
	}

	snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%dp", card, dev);
	int fd = open(path, O_WRONLY);
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
	set_int(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
	set_int(&hp, SNDRV_PCM_HW_PARAM_PERIODS, PERIODS);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) { perror("HW_PARAMS (S16_LE mono 16000)"); return 1; }
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { perror("PREPARE"); return 1; }

	/* what has been played is overwritten with silence, so an underrun or the
	 * ragged end of the last period plays silence and not old sound */
	struct snd_pcm_sw_params sp;
	memset(&sp, 0, sizeof(sp));
	sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sp.period_step = 1;
	sp.avail_min = PERIOD;
	sp.xfer_align = 1;
	sp.start_threshold = PERIOD * PERIODS;
	sp.stop_threshold = PERIOD * PERIODS;
	sp.silence_threshold = 0;
	sp.silence_size = ~0UL >> 1;
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) < 0) { perror("SW_PARAMS"); return 1; }

	/* blocking writes; the stream starts by itself once the buffer is full */
	double t0 = now(), t1;
	while (done < total) {
		unsigned want = total - done < PERIOD ? total - done : PERIOD;
		int n = write(fd, pcm + done, want * 2);
		if (n < 0 && errno == EPIPE) {
			underruns++;
			if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { perror("PREPARE after underrun"); break; }
			continue;
		}
		if (n <= 0) { perror("write"); break; }
		done += n / 2;
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_DRAIN) < 0 && errno != EPIPE)
		perror("DRAIN");
	t1 = now();
	close(fd);
	printf("%s: %u frames in %.3f s (%.1f frames/s); underruns %u\n", path, done, t1 - t0,
	       done / (t1 - t0), underruns);
	return 0;
}
