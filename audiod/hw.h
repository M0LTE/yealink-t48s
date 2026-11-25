/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48-audiod: the hardware side (issue #24). One interface, three backends:
 *   null  no hardware: a 20 ms timer, silence in, sound thrown away
 *   file  the same timer, microphone read from a WAV file, loudspeaker
 *         written to one (tests, and development on a PC)
 *   alsa  the phone's codecs through the kernel's ALSA ioctls, no alsa-lib
 *
 * The backend is the clock. While it runs, tick_fd() becomes readable every
 * 20 ms; the daemon then calls capture() until it returns 0, and playback()
 * once for every frame captured. On the phone both directions are driven by
 * the codec's own sample clock, so they cannot drift apart.
 */
#ifndef AUDIOD_HW_H
#define AUDIOD_HW_H
#include <stdint.h>
#include "routes.h"

struct hw {
	const char *name;
	/* Set the hardware up for this route and start the clock. Called
	 * again with another route to switch. 0 or -1. */
	int (*start)(struct hw *hw, const struct route *r);
	/* Stop everything and close the devices. No ticks until start. */
	void (*stop)(struct hw *hw);
	/* To poll for POLLIN; -1 while stopped */
	int (*tick_fd)(struct hw *hw);
	/* One microphone frame (FRAME samples): 1, or 0 if none is due yet,
	 * or -1 if the hardware failed (the daemon stops and restarts it) */
	int (*capture)(struct hw *hw, int16_t *mic);
	/* One loudspeaker frame. Never blocks. */
	void (*playback)(struct hw *hw, const int16_t *spk);
	void (*destroy)(struct hw *hw);
	/* T48AC_HW_CAPTURE | T48AC_HW_PLAYBACK: what the last start found */
	int caps;
	/* samples between playback() and the loudspeaker, more or less */
	int play_delay;
	/* counters for the log */
	unsigned long cap_xruns, play_xruns, play_dropped;
};

/* room_ms, room_percent: 0, or an echo for tests: what is played returns to
 * the microphone that much later at that fraction of its level */
struct hw *hw_null_new(const char *mic_wav, const char *spk_wav, int room_ms, int room_percent);
struct hw *hw_alsa_new(int card);

#endif
