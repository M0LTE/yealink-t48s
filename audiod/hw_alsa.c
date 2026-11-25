// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-audiod: the phone's codecs through ALSA (kernel/drivers/t48s_audio.c),
 * with the kernel's own ioctls and no alsa-lib, like native/t48rec.c and
 * native/t48mix.c.
 *
 * Capture is the clock: the capture device is opened with 20 ms periods and
 * polled, so a frame is due exactly when the codec has made one. Playback is
 * written one frame for each frame captured, into a buffer that starts with
 * 40 ms in it; both run off the same sample clock, so that level holds.
 *
 * Played frames are replaced with silence in the buffer as they go (ALSA's
 * silence fill), so if the service is ever late the codec plays silence, not
 * the last 80 ms again; the driver then fades and idles on zeros until the
 * stream is prepared and written again.
 *
 * A kernel without playback (the first capture-only driver) is reported once
 * and the service runs with the microphone only.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sound/asound.h>
#include "dsp.h"
#include "hw.h"

#define PERIODS       4			/* 80 ms of buffer each way */
#define PLAY_START    (2 * FRAME)	/* playback starts with 40 ms queued */
#define MAX_CTLS      96
#define MAX_WARNED    48

struct hw_alsa {
	struct hw hw;
	int card;
	int ctl_fd, cap_fd, play_fd;
	int16_t part[FRAME];	/* a capture frame arriving in pieces */
	size_t part_bytes;
	struct snd_ctl_elem_id ids[MAX_CTLS];
	unsigned nids;
	char warned[MAX_WARNED][44];
	unsigned nwarned;
	int play_warned;
	int silent, measure;
};

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

/* Open one direction of one device: S16_LE, mono, 16 kHz, 20 ms periods */
static int pcm_open(int card, int dev, int play)
{
	struct snd_pcm_hw_params hp;
	char path[40];
	int fd, i;

	snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%d%c", card, dev, play ? 'p' : 'c');
	fd = open(path, (play ? O_WRONLY : O_RDONLY) | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	memset(&hp, 0, sizeof(hp));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
		memset(&hp.masks[i], 0xff, sizeof(hp.masks[i]));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++)
		hp.intervals[i].max = ~0u;
	hp.rmask = ~0u;
	set_mask(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	set_mask(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	set_int(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 1);
	set_int(&hp, SNDRV_PCM_HW_PARAM_RATE, SRATE);
	set_int(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, FRAME);
	set_int(&hp, SNDRV_PCM_HW_PARAM_PERIODS, PERIODS);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0)
		goto fail;
	if (play) {
		struct snd_pcm_sw_params sp;

		memset(&sp, 0, sizeof(sp));
		sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
		sp.period_step = 1;
		sp.avail_min = FRAME;
		sp.xfer_align = 1;
		sp.start_threshold = PLAY_START;
		sp.stop_threshold = FRAME * PERIODS;	/* stop when it runs dry */
		sp.silence_threshold = 0;		/* and keep what has been played */
		sp.silence_size = ~0UL >> 1;		/* overwritten with silence */
		if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) < 0)
			goto fail;
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
		goto fail;
	if (!play && ioctl(fd, SNDRV_PCM_IOCTL_START) < 0)
		goto fail;
	return fd;
fail:
	fprintf(stderr, "t48-audiod: %s: %s\n", path, strerror(errno));
	close(fd);
	return -1;
}

static void pcm_close(int *fd)
{
	if (*fd < 0)
		return;
	ioctl(*fd, SNDRV_PCM_IOCTL_DROP);
	close(*fd);
	*fd = -1;
}

static void ctl_list(struct hw_alsa *a)
{
	struct snd_ctl_elem_list list;

	memset(&list, 0, sizeof(list));
	list.space = MAX_CTLS;
	list.pids = a->ids;
	a->nids = 0;
	if (ioctl(a->ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0)
		a->nids = list.used < MAX_CTLS ? list.used : MAX_CTLS;
}

static void ctl_set(struct hw_alsa *a, const char *name, int value)
{
	struct snd_ctl_elem_info info;
	struct snd_ctl_elem_value val;
	unsigned i;

	for (i = 0; i < a->nids; i++)
		if (!strcmp((const char *)a->ids[i].name, name))
			break;
	if (i == a->nids) {
		for (i = 0; i < a->nwarned; i++)
			if (!strcmp(a->warned[i], name))
				return;
		if (a->nwarned < MAX_WARNED)
			snprintf(a->warned[a->nwarned++], sizeof(a->warned[0]), "%s", name);
		fprintf(stderr, "t48-audiod: the kernel has no control \"%s\": skipped\n", name);
		return;
	}
	/* --silent: nothing that lets sound out is ever switched on */
	if (a->silent && value && (!strcmp(name, "Speaker Amp Switch") || strstr(name, "Playback Switch") ||
				   strstr(name, "Sidetone Switch")))
		value = 0;
	memset(&info, 0, sizeof(info));
	memset(&val, 0, sizeof(val));
	info.id = val.id = a->ids[i];
	if (ioctl(a->ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
		return;
	if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
		val.value.enumerated.item[0] = (unsigned)value;
	else
		val.value.integer.value[0] = value;
	if (ioctl(a->ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0)
		fprintf(stderr, "t48-audiod: control \"%s\" = %d: %s\n", name, value, strerror(errno));
}

static void streams_close(struct hw_alsa *a)
{
	pcm_close(&a->play_fd);
	pcm_close(&a->cap_fd);
	a->part_bytes = 0;
}

/* Idle means silent and deaf: amplifier, outputs, sidetone and microphones
 * off, whatever the last route left on. */
static void alsa_stop(struct hw *hw)
{
	static const char *const off[] = {
		"Speaker Amp Switch",
		"Codec A Output A Playback Switch", "Codec A Output B Playback Switch",
		"Codec B Output A Playback Switch", "Codec B Output B Playback Switch",
		"Codec A Sidetone Switch", "Codec B Sidetone Switch",
		"Codec A Capture Switch", "Codec B Capture Switch",
	};
	struct hw_alsa *a = (struct hw_alsa *)hw;
	unsigned i;

	streams_close(a);
	if (a->ctl_fd >= 0)
		for (i = 0; i < sizeof(off) / sizeof(off[0]); i++)
			ctl_set(a, off[i], 0);
}

static int alsa_start(struct hw *hw, const struct route *r)
{
	struct hw_alsa *a = (struct hw_alsa *)hw;
	static const int16_t silence[FRAME];
	int i;

	/* Streams off first, so nothing is heard while the paths change */
	streams_close(a);
	if (a->ctl_fd < 0) {
		char path[40];
		snprintf(path, sizeof(path), "/dev/snd/controlC%d", a->card);
		a->ctl_fd = open(path, O_RDWR | O_CLOEXEC);
		if (a->ctl_fd < 0) {
			fprintf(stderr, "t48-audiod: %s: %s\n", path, strerror(errno));
			return -1;
		}
		ctl_list(a);
	}
	for (i = 0; i < r->nctl; i++)
		ctl_set(a, r->ctl[i].name, r->ctl[i].value);

	hw->caps = 0;
	a->cap_fd = pcm_open(a->card, r->cap_dev, 0);
	if (a->cap_fd < 0)
		return -1;	/* no clock without capture */
	hw->caps |= T48AC_HW_CAPTURE;

	a->play_fd = pcm_open(a->card, r->play_dev, 1);
	if (a->play_fd >= 0) {
		hw->caps |= T48AC_HW_PLAYBACK;
		/* one frame of silence: with the first real frame that makes
		 * the 40 ms the stream starts at */
		if (write(a->play_fd, silence, sizeof(silence)) < 0)
			hw->play_dropped++;
	} else if (!a->play_warned) {
		a->play_warned = 1;
		fprintf(stderr, "t48-audiod: no playback device: running with the microphone only\n");
	}
	return 0;
}

static int alsa_tick_fd(struct hw *hw)
{
	return ((struct hw_alsa *)hw)->cap_fd;
}

static int alsa_capture(struct hw *hw, int16_t *mic)
{
	struct hw_alsa *a = (struct hw_alsa *)hw;

	if (a->cap_fd < 0)
		return -1;
	for (;;) {
		ssize_t n = read(a->cap_fd, (char *)a->part + a->part_bytes,
				 sizeof(a->part) - a->part_bytes);
		if (n > 0) {
			a->part_bytes += (size_t)n;
			if (a->part_bytes < sizeof(a->part))
				continue;
			memcpy(mic, a->part, sizeof(a->part));
			a->part_bytes = 0;
			return 1;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return 0;
		if (n < 0 && errno == EPIPE) {
			/* overrun: we were not scheduled for 80 ms. Start again. */
			hw->cap_xruns++;
			a->part_bytes = 0;
			if (ioctl(a->cap_fd, SNDRV_PCM_IOCTL_PREPARE) < 0 ||
			    ioctl(a->cap_fd, SNDRV_PCM_IOCTL_START) < 0)
				return -1;
			return 0;
		}
		return -1;
	}
}

static void alsa_playback(struct hw *hw, const int16_t *spk)
{
	struct hw_alsa *a = (struct hw_alsa *)hw;
	static const int16_t silence[FRAME];
	ssize_t n;

	if (a->play_fd < 0)
		return;
	n = write(a->play_fd, spk, FRAME * sizeof(int16_t));
	if (n == (ssize_t)(FRAME * sizeof(int16_t))) {
		snd_pcm_sframes_t d;

		if (a->measure && ioctl(a->play_fd, SNDRV_PCM_IOCTL_DELAY, &d) == 0 && d >= 0) {
			if (!hw->depth_n || (unsigned long)d < hw->depth_min)
				hw->depth_min = (unsigned long)d;
			if ((unsigned long)d > hw->depth_max)
				hw->depth_max = (unsigned long)d;
			hw->depth_sum += (unsigned long)d;
			hw->depth_n++;
		}
		return;
	}
	if (n < 0 && errno == EPIPE) {
		/* ran dry: start again with the same 40 ms */
		hw->play_xruns++;
		if (ioctl(a->play_fd, SNDRV_PCM_IOCTL_PREPARE) == 0 &&
		    write(a->play_fd, silence, sizeof(silence)) > 0 &&
		    write(a->play_fd, spk, FRAME * sizeof(int16_t)) > 0)
			return;
	}
	/* full (EAGAIN), a short write, or the restart failed: this frame is lost */
	hw->play_dropped++;
}

static void alsa_destroy(struct hw *hw)
{
	struct hw_alsa *a = (struct hw_alsa *)hw;
	alsa_stop(hw);
	if (a->ctl_fd >= 0)
		close(a->ctl_fd);
	free(a);
}

struct hw *hw_alsa_new(int card, int silent, int measure)
{
	struct hw_alsa *a = calloc(1, sizeof(*a));

	if (!a)
		return 0;
	a->card = card;
	a->silent = silent;
	a->measure = measure;
	a->ctl_fd = a->cap_fd = a->play_fd = -1;
	a->hw.name = "alsa";
	/* a frame is written behind one already queued, and the driver moves
	 * 5 ms at a time */
	a->hw.play_delay = PLAY_START - FRAME + 80;
	a->hw.start = alsa_start;
	a->hw.stop = alsa_stop;
	a->hw.tick_fd = alsa_tick_fd;
	a->hw.capture = alsa_capture;
	a->hw.playback = alsa_playback;
	a->hw.destroy = alsa_destroy;
	return &a->hw;
}
