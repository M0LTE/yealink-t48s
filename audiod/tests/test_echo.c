// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod test: the speakerphone's voice switching, against a made-up
 * room: the microphone hears the loudspeaker 60 ms late at half level, plus
 * a little background noise. */
#include <string.h>
#include "t.h"
#include "../dsp.h"
#include "../echo.h"

#define SEC   SRATE
#define DELAY (3 * FRAME)	/* loudspeaker to microphone, with buffering */
#define TOTAL (6 * SEC)

static int16_t far[TOTAL], near[TOTAL], played[TOTAL + DELAY], sent[TOTAL];
static enum echo_state state_at[TOTAL / FRAME];
static int room = 1;	/* 0: a room with no echo at all */

/* Run the whole thing a frame at a time. far: what the call sends us.
 * near: the owner's voice at the microphone. */
static void run(struct echo *e)
{
	uint32_t seed = 7;
	int16_t spk[FRAME], mic[FRAME], bg[FRAME];
	int t, i;

	memset(played, 0, sizeof(played));
	for (t = 0; t < TOTAL; t += FRAME) {
		memcpy(spk, far + t, sizeof(spk));
		noise_fill(bg, FRAME, 30, &seed);
		for (i = 0; i < FRAME; i++)
			mic[i] = sat16(near[t + i] + bg[i] + (room && t + i >= DELAY ? played[t + i - DELAY] / 2 : 0));
		echo_process(e, spk, mic, FRAME);
		memcpy(played + t, spk, sizeof(spk));
		memcpy(sent + t, mic, sizeof(mic));
		state_at[t / FRAME] = e->state;
	}
}

static double db(double a, double b)
{
	return 20 * log10((a + 1e-9) / (b + 1e-9));
}

int main(void)
{
	struct echo e;
	uint32_t seed = 1;
	int i;

	/* mode none: nothing is touched */
	echo_init(&e, ECHO_NONE, 0);
	noise_fill(far, SEC, 8000, &seed);
	noise_fill(near, SEC, 8000, &seed);
	memcpy(played, far, FRAME * 2);
	memcpy(sent, near, FRAME * 2);
	echo_process(&e, played, sent, FRAME);
	CHECK(!memcmp(played, far, FRAME * 2) && !memcmp(sent, near, FRAME * 2));

	/* 1. Only the far end talks (seconds 1 to 3). Its echo must not go
	 * back: at least 30 dB down on what the microphone picked up. */
	memset(far, 0, sizeof(far));
	memset(near, 0, sizeof(near));
	noise_fill(far + SEC, 2 * SEC, 8000, &seed);
	echo_init(&e, ECHO_SWITCH, 256);
	run(&e);
	{
		double echo_in = rms(played + SEC + SEC / 2, SEC) / 2;	/* what the room returned */
		double echo_out = rms(sent + SEC + SEC / 2 + DELAY, SEC);
		printf("  far end only: echo at the microphone %.0f, sent to the call %.0f (%.1f dB)\n",
		       echo_in, echo_out, db(echo_out, echo_in));
		CHECK(db(echo_out, echo_in) < -30);
		CHECK(state_at[(SEC + SEC / 2) / FRAME] == ECHO_RX);
		/* the loudspeaker itself is not turned down */
		CHECK_RANGE(db(rms(played + SEC + SEC / 2, SEC), rms(far + SEC + SEC / 2, SEC)), -0.1, 0.1);
		/* the very start of the echo is caught too: the microphone is
		 * already down before the sound has crossed the room */
		CHECK(rms(sent + SEC + DELAY, FRAME) < echo_in / 20);
		/* and the tail after the far end stops (60 ms of delay to cover) */
		CHECK(rms(sent + 3 * SEC, DELAY + FRAME) < echo_in / 20);
		/* then the microphone opens again: within 0.6 s of the far end stopping */
		CHECK(state_at[(3 * SEC + SEC * 6 / 10) / FRAME] == ECHO_IDLE);
		CHECK(e.mic_g == Q15_ONE);
		/* what the far end hears meanwhile is the background, not a dead line */
		CHECK_RANGE(rms(sent + 2 * SEC, SEC / 2), 5, 60);
	}

	/* 2. Only the owner talks (seconds 1 to 3): passes untouched, from
	 * the first frame */
	memset(far, 0, sizeof(far));
	memset(near, 0, sizeof(near));
	noise_fill(near + SEC, 2 * SEC, 6000, &seed);
	echo_init(&e, ECHO_SWITCH, 256);
	run(&e);
	CHECK_RANGE(db(rms(sent + SEC, 2 * SEC), rms(near + SEC, 2 * SEC)), -0.2, 0.2);
	CHECK_RANGE(db(rms(sent + SEC, FRAME), rms(near + SEC, FRAME)), -0.5, 0.5);
	CHECK(state_at[(2 * SEC) / FRAME] == ECHO_TX);

	/* 3. The far end is talking (1 to 5) and the owner talks over it,
	 * louder (2 to 3): the owner gets through, the loudspeaker drops 15 dB
	 * so less echo goes with it, and afterwards the far end has the
	 * channel back */
	memset(far, 0, sizeof(far));
	memset(near, 0, sizeof(near));
	noise_fill(far + SEC, 4 * SEC, 4000, &seed);
	noise_fill(near + 2 * SEC, SEC, 24000, &seed);
	echo_init(&e, ECHO_SWITCH, 256);
	run(&e);
	CHECK(state_at[(SEC + SEC / 2) / FRAME] == ECHO_RX);
	CHECK(state_at[(2 * SEC + SEC / 2) / FRAME] == ECHO_TX);
	CHECK(db(rms(sent + 2 * SEC + SEC / 10, SEC * 8 / 10), rms(near + 2 * SEC + SEC / 10, SEC * 8 / 10)) > -1.5);
	CHECK_RANGE(db(rms(played + 2 * SEC + SEC / 4, SEC / 2), rms(far + 2 * SEC + SEC / 4, SEC / 2)), -15.5, -14.5);
	CHECK(state_at[(4 * SEC) / FRAME] == ECHO_RX);
	CHECK(db(rms(sent + 4 * SEC, SEC / 2), rms(played + 4 * SEC, SEC / 2) / 2) < -30);
	/* breaking in takes no more than 40 ms to open the microphone */
	for (i = 2 * SEC / FRAME; state_at[i] != ECHO_TX; i++)
		;
	CHECK(i - 2 * SEC / FRAME <= 1);

	/* 4. A quiet owner under a loud far end does not break in: that is
	 * the price of this method, and why the level setting matters */
	memset(near, 0, sizeof(near));
	noise_fill(near + 2 * SEC, SEC, 1500, &seed);
	echo_init(&e, ECHO_SWITCH, 256);
	run(&e);
	CHECK(state_at[(2 * SEC + SEC / 2) / FRAME] == ECHO_RX);

	/* 5. Gains move smoothly: a steady tone through a switch has no step */
	for (i = 0; i < TOTAL; i++)
		near[i] = (int16_t)(3000 * sin(2 * M_PI * 200 * i / 16000.0));
	memset(far, 0, sizeof(far));
	noise_fill(far + SEC, SEC, 8000, &seed);
	echo_init(&e, ECHO_SWITCH, 2560);	/* strong coupling: the far end wins */
	room = 0;	/* so that only the tone is in what is sent */
	run(&e);
	room = 1;
	CHECK(state_at[(SEC + SEC / 2) / FRAME] == ECHO_RX);
	/* a gain jump would be a step of up to 3000; the tone's own is 236 and
	 * the comfort noise adds up to 1200 */
	CHECK(max_step(sent, TOTAL) < 1500);

	/* 6. Background noise alone never switches anything */
	memset(far, 0, sizeof(far));
	memset(near, 0, sizeof(near));
	echo_init(&e, ECHO_SWITCH, 256);
	run(&e);
	for (i = 0; i < TOTAL / FRAME; i++)
		if (state_at[i] != ECHO_IDLE)
			break;
	CHECK(i == TOTAL / FRAME);

	T_DONE("echo");
}
