// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: an app's sound. See appsnd.h. */
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "appsnd.h"

#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#define F_SEAL_SHRINK 0x0002
#endif

/* a part frame is played after waiting this many ticks for the rest of it */
#define SHORT_TICKS 2

int appsnd_open(struct appsnd *a, int fd)
{
	struct stat st;
	int seals = fcntl(fd, F_GET_SEALS);
	void *m;

	appsnd_close(a);
	/* F_SEAL_SHRINK is what makes the mapping safe to read: without it the
	 * app could cut the file short and the next tick would be a SIGBUS */
	if (seals < 0 || !(seals & F_SEAL_SHRINK) || fstat(fd, &st) < 0 || st.st_size < T48AC_APP_BYTES) {
		close(fd);
		return -1;
	}
	m = mmap(0, T48AC_APP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return -1;
	memset(a, 0, sizeof(*a));
	a->ring = m;
	a->data = (const int16_t *)((const char *)m + T48AC_APP_DATA);
	a->rd = a->ring->wr;	/* start empty, whatever is in there */
	a->ring->rd = a->rd;
	a->ring->state = T48_SOUND_OFF;
	return 0;
}

void appsnd_close(struct appsnd *a)
{
	if (!a->ring)
		return;
	a->ring->state = T48_SOUND_OFF;
	munmap(a->ring, T48AC_APP_BYTES);
	a->ring = 0;
	a->front = a->playing = 0;
}

static void wake(struct appsnd *a)
{
	if (!a->ring->waiting)
		return;
	a->ring->waiting = 0;
	syscall(SYS_futex, &a->ring->rd, FUTEX_WAKE, INT_MAX, 0, 0, 0);
}

/* Samples waiting. A count that cannot be (more than the ring holds: the app
 * has scribbled on it, or lapped itself) empties the ring. */
static uint32_t waiting(struct appsnd *a)
{
	uint32_t n = a->ring->wr - a->rd;

	if (n > T48AC_APP_FRAMES) {
		a->rd = a->ring->wr;
		a->resets++;
		n = 0;
	}
	return n;
}

void appsnd_publish(struct appsnd *a, int held, int delay)
{
	if (!a->ring)
		return;
	a->ring->rd = a->rd;
	a->ring->delay = (uint32_t)delay;
	a->ring->state = !a->front ? T48_SOUND_OFF : held ? T48_SOUND_HELD : T48_SOUND_PLAYING;
	wake(a);
}

void appsnd_front(struct appsnd *a, int on)
{
	if (!a->ring)
		return;
	a->front = !!on;
	a->playing = a->short_ticks = 0;
	a->rd = a->ring->wr;	/* nothing stale is played, now or when it comes back */
	appsnd_publish(a, 0, 0);
}

int appsnd_waiting(struct appsnd *a)
{
	return a->ring && a->front ? (int)waiting(a) : 0;
}

int appsnd_take(struct appsnd *a, int16_t *out, int held, int delay)
{
	uint32_t n, i, at;

	if (!a->ring || !a->front)
		return 0;
	n = waiting(a);
	if (!a->playing) {
		/* start on a whole frame, so the first tick is not half silence;
		 * a sound shorter than that is not kept waiting long */
		if (n >= FRAME || (n && ++a->short_ticks > SHORT_TICKS))
			a->playing = 1;
		else if (!n)
			a->short_ticks = 0;
	}
	if (!a->playing) {
		appsnd_publish(a, held, delay);
		return 0;
	}
	if (n > FRAME)
		n = FRAME;
	at = a->rd & (T48AC_APP_FRAMES - 1);
	for (i = 0; i < n; i++) {
		int v = a->data[(at + i) & (T48AC_APP_FRAMES - 1)];

		out[i] = (int16_t)v;
		if (v < 0)
			v = -v;
		if (v > a->peak)
			a->peak = v;
	}
	memset(out + n, 0, (FRAME - n) * sizeof(int16_t));
	a->rd += n;
	a->samples += n;
	a->frames++;
	if (n < FRAME) {
		/* ran dry: silence from here, and the next sound starts afresh */
		a->playing = a->short_ticks = 0;
		a->dry++;
		a->ring->dry++;
	}
	appsnd_publish(a, held, delay);
	return n > 0;
}
