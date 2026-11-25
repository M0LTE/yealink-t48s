/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48-audiod: an app's sound (issue #63, docs/audiod.md). The app writes
 * samples into a ring in memory it shares with us (struct t48_sound_ring);
 * each tick we take one frame out. No socket, no copy but that one.
 *
 * The app is not trusted. The ring is a memfd sealed against shrinking, so
 * reading it cannot fault; nothing the app can write into it is believed:
 * the size and place of the samples are constants here, our own count of
 * what has been taken is kept here, and a write count that makes no sense
 * empties the ring.
 */
#ifndef AUDIOD_APPSND_H
#define AUDIOD_APPSND_H
#include <stdint.h>
#include "dsp.h"
#include "t48_proto.h"
#include "include/t48_audio_ctl.h"

struct appsnd {
	volatile struct t48_sound_ring *ring;	/* NULL: no app has sound; the app writes to it as we read */
	const int16_t *data;
	int front;		/* the app is on the screen */
	int playing;		/* frames are being taken; 0 until one is ready */
	int short_ticks;	/* ticks a part frame has waited for the rest */
	uint32_t rd;		/* samples taken: ours, the ring's copy is for the app */
	/* for the log */
	unsigned long frames, samples, dry, resets;
	int peak;
};

/* Take over the ring behind fd (which is closed, either way): 0, or -1 if it
 * is not a sealed memfd of the right size. Any ring held before is let go. */
int appsnd_open(struct appsnd *a, int fd);
void appsnd_close(struct appsnd *a);
/* In front or not. Going off the screen drops what is waiting. */
void appsnd_front(struct appsnd *a, int on);
/* Samples waiting to be played, 0 when not in front */
int appsnd_waiting(struct appsnd *a);
/* One tick: 1 and a frame (the end filled with silence), or 0 if there is
 * nothing to play. held: what goes into the ring's state, for the app.
 * delay: samples from here to the loudspeaker, likewise. */
int appsnd_take(struct appsnd *a, int16_t *out, int held, int delay);
/* While the hardware is stopped: say so in the ring (nothing is taken) */
void appsnd_publish(struct appsnd *a, int held, int delay);

#endif
