/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48-audiod: echo control (issue #24, reasons in docs/audiod.md).
 *
 * Handset and headset need none: the earpiece barely reaches the microphone.
 * The speakerphone uses voice switching, the way analogue speakerphones do:
 * whichever side is talking gets the channel and the other is turned down.
 * While the far end talks the microphone is cut by 36 dB (with comfort noise
 * in its place); when the owner talks over it the microphone opens and the
 * loudspeaker drops 15 dB. It costs a few passes over the frame, where a
 * real echo canceller costs tens of times more, and it has nothing to learn
 * about the room, so it cannot be tuned wrong. A canceller can replace it
 * behind the same call later if the phone has the CPU to spare.
 */
#ifndef AUDIOD_ECHO_H
#define AUDIOD_ECHO_H
#include <stdint.h>

enum echo_mode {
	ECHO_NONE = 0,
	ECHO_SWITCH = 1,	/* voice-switched speakerphone */
};

enum echo_state { ECHO_IDLE, ECHO_RX, ECHO_TX };

#define ECHO_HIST 6	/* frames of loudspeaker level remembered: 120 ms, the
			 * playback delay plus the start of the room's echo */

struct echo {
	enum echo_mode mode;
	int coupling_q8;	/* echo level at the microphone for a given
				 * loudspeaker level, 256 = equal. From the route
				 * table; to be measured on the phone. */
	enum echo_state state;
	int hist[ECHO_HIST];	/* level played in each recent frame */
	int hist_pos;
	int tail;		/* decaying memory of older frames (reverberation) */
	int mic_noise_q4;	/* background level at the microphone */
	int tx_hold;		/* frames the owner keeps the channel after speaking */
	int32_t mic_g, spk_g;	/* gains now, Q15, slewed per sample */
	uint32_t rng;
};

void echo_init(struct echo *e, enum echo_mode mode, int coupling_q8);

/* One frame. spk is what is about to be played (after volume), mic what was
 * just captured; both are changed in place. */
void echo_process(struct echo *e, int16_t *spk, int16_t *mic, int n);

#endif
