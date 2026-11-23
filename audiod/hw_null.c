// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-audiod: the null and file backends. A timerfd stands in for the codec's
 * clock. With no files it is the null backend. With a microphone file, that
 * file (WAV, 16 kHz mono 16-bit) is what the microphone hears, round and
 * round; with a loudspeaker file, everything played is written to it as a
 * WAV. Time in the loudspeaker file is the time the hardware ran: nothing is
 * written while the service is idle.
 *
 * For testing echo control it can also be a room: what was played comes back
 * into the microphone a set time later at a set fraction of its level.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>
#include "dsp.h"
#include "hw.h"

struct hw_null {
	struct hw hw;
	int tfd;
	unsigned due;		/* ticks the timer says have passed, not yet taken */
	int16_t *mic;		/* the whole microphone file */
	size_t mic_len, mic_pos;
	FILE *spk;
	uint32_t spk_bytes;
	/* the room: a ring of what was played */
	int16_t *room;
	unsigned room_len, room_pos;	/* samples of delay; where the next goes */
	int room_gain_q8;
};

static void wav_header(FILE *f, uint32_t bytes)
{
	uint32_t h[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16,
			   0x00010001, SRATE, SRATE * 2, 0x00100002, 0x61746164, bytes };
	fwrite(h, 4, 11, f);	/* every machine we build for is little-endian */
}

static void spk_flush_header(struct hw_null *n)
{
	if (!n->spk)
		return;
	fflush(n->spk);
	if (!fseek(n->spk, 0, SEEK_SET)) {
		wav_header(n->spk, n->spk_bytes);
		fflush(n->spk);
		fseek(n->spk, 0, SEEK_END);
	}
}

static int null_start(struct hw *hw, const struct route *r)
{
	struct hw_null *n = (struct hw_null *)hw;
	struct itimerspec its = { { 0, FRAME_MS * 1000000L }, { 0, FRAME_MS * 1000000L } };

	(void)r;
	if (n->tfd >= 0)
		return 0;	/* a route change: nothing to do, keep the clock */
	n->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (n->tfd < 0 || timerfd_settime(n->tfd, 0, &its, 0) < 0) {
		perror("t48-audiod: timerfd");
		return -1;
	}
	n->due = 0;
	return 0;
}

static void null_stop(struct hw *hw)
{
	struct hw_null *n = (struct hw_null *)hw;
	if (n->tfd >= 0)
		close(n->tfd);
	n->tfd = -1;
	spk_flush_header(n);
}

static int null_tick_fd(struct hw *hw)
{
	return ((struct hw_null *)hw)->tfd;
}

static int null_capture(struct hw *hw, int16_t *mic)
{
	struct hw_null *n = (struct hw_null *)hw;
	uint64_t expired;
	int i;

	if (n->tfd < 0)
		return -1;
	if (!n->due && read(n->tfd, &expired, sizeof(expired)) == sizeof(expired)) {
		/* After a long stall (the process was stopped) do not replay
		 * every missed tick: hardware would have overrun and dropped */
		if (expired > 5) {
			hw->cap_xruns++;
			expired = 1;
		}
		n->due = (unsigned)expired;
	}
	if (!n->due)
		return 0;
	n->due--;
	if (n->mic_len) {
		for (i = 0; i < FRAME; i++) {
			mic[i] = n->mic[n->mic_pos];
			if (++n->mic_pos == n->mic_len)
				n->mic_pos = 0;
		}
	} else {
		memset(mic, 0, FRAME * sizeof(int16_t));
	}
	if (n->room) {
		/* room[pos] is the oldest sample: played room_len samples ago */
		unsigned p = n->room_pos;
		for (i = 0; i < FRAME; i++) {
			mic[i] = sat16(mic[i] + ((n->room[p] * n->room_gain_q8) >> 8));
			if (++p == n->room_len)
				p = 0;
		}
	}
	return 1;
}

static void null_playback(struct hw *hw, const int16_t *spk)
{
	struct hw_null *n = (struct hw_null *)hw;
	int i;

	if (n->spk) {
		fwrite(spk, sizeof(int16_t), FRAME, n->spk);
		n->spk_bytes += FRAME * sizeof(int16_t);
	}
	if (n->room) {
		for (i = 0; i < FRAME; i++) {
			n->room[n->room_pos] = spk[i];
			if (++n->room_pos == n->room_len)
				n->room_pos = 0;
		}
	}
}

static void null_destroy(struct hw *hw)
{
	struct hw_null *n = (struct hw_null *)hw;
	null_stop(hw);
	if (n->spk)
		fclose(n->spk);
	free(n->mic);
	free(n->room);
	free(n);
}

/* Load a 16 kHz mono 16-bit WAV. Returns 0 or -1. */
static int load_wav(struct hw_null *n, const char *path)
{
	FILE *f = fopen(path, "rb");
	unsigned char h[12], c[8];
	uint32_t len, fmt_ok = 0;

	if (!f) {
		perror(path);
		return -1;
	}
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
		goto bad;
	while (fread(c, 1, 8, f) == 8) {
		len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
		if (!memcmp(c, "fmt ", 4)) {
			unsigned char fm[16];
			if (len < 16 || fread(fm, 1, 16, f) != 16)
				goto bad;
			/* PCM, 1 channel, 16000 Hz, 16 bits */
			fmt_ok = fm[0] == 1 && fm[2] == 1 && fm[14] == 16 &&
				 (fm[4] | fm[5] << 8 | fm[6] << 16) == SRATE;
			fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR);
		} else if (!memcmp(c, "data", 4)) {
			if (!fmt_ok || len < 2)
				goto bad;
			n->mic = malloc(len);
			if (!n->mic)
				goto bad;
			n->mic_len = fread(n->mic, 2, len / 2, f);
			fclose(f);
			return n->mic_len ? 0 : -1;
		} else {
			fseek(f, (long)(len + (len & 1)), SEEK_CUR);
		}
	}
bad:
	fprintf(stderr, "t48-audiod: %s: need a WAV file, 16000 Hz, mono, 16-bit\n", path);
	fclose(f);
	return -1;
}

struct hw *hw_null_new(const char *mic_wav, const char *spk_wav, int room_ms, int room_percent)
{
	struct hw_null *n = calloc(1, sizeof(*n));

	if (!n)
		return 0;
	n->tfd = -1;
	n->hw.name = (mic_wav || spk_wav || room_ms) ? "file" : "null";
	n->hw.start = null_start;
	n->hw.stop = null_stop;
	n->hw.tick_fd = null_tick_fd;
	n->hw.capture = null_capture;
	n->hw.playback = null_playback;
	n->hw.destroy = null_destroy;
	n->hw.caps = T48AC_HW_CAPTURE | T48AC_HW_PLAYBACK;
	if (room_ms > 0 && room_percent > 0) {
		/* whole frames, so capture and playback stay in step */
		n->room_len = (unsigned)((room_ms + FRAME_MS - 1) / FRAME_MS) * FRAME;
		n->room = calloc(n->room_len, sizeof(int16_t));
		n->room_gain_q8 = room_percent * 256 / 100;
	}
	if (mic_wav && load_wav(n, mic_wav) < 0) {
		free(n);
		return 0;
	}
	if (spk_wav) {
		n->spk = fopen(spk_wav, "wb");
		if (!n->spk) {
			perror(spk_wav);
			free(n->mic);
			free(n);
			return 0;
		}
		wav_header(n->spk, 0);
	}
	return &n->hw;
}
