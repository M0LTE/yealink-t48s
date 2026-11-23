// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod test: "echo aec", the echo canceller with the switch behind it
 * (issue #35), against a made-up room: the microphone hears the loudspeaker
 * 60 ms late at half level and again 15 ms after that at a quarter, plus a
 * little background noise. The far end is loud enough here that its echo
 * drowns the owner, which is where voice switching alone gives up. */
#include <string.h>
#include "t.h"
#include "../dsp.h"
#include "../echo.h"
#include "../engine.h"

#define SEC   SRATE
#define DELAY (3 * FRAME)
#define TOTAL (10 * SEC)

static int16_t far[TOTAL], near[TOTAL], played[TOTAL], sent[TOTAL];
static enum echo_state state_at[TOTAL / FRAME];
static int adapted_at[TOTAL / FRAME];

static int room(int i)
{
	int v = 0;
	if (i >= DELAY)
		v += played[i - DELAY] / 2;
	if (i >= DELAY + 240)
		v -= played[i - DELAY - 240] / 4;
	return v;
}

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
			mic[i] = sat16(near[t + i] + bg[i] + room(t + i));
		echo_process(e, spk, mic, FRAME);
		memcpy(played + t, spk, sizeof(spk));
		memcpy(sent + t, mic, sizeof(mic));
		state_at[t / FRAME] = e->state;
		adapted_at[t / FRAME] = e->adapted;
	}
}

static double db(double a, double b)
{
	return 20 * log10((a + 1e-9) / (b + 1e-9));
}

/* The echo of played[a..a+n) as the room returns it */
static double echo_rms(int a, int n)
{
	double s = 0;
	int i;
	for (i = a; i < a + n; i++)
		s += (double)room(i) * room(i);
	return sqrt(s / n);
}

int main(void)
{
	struct echo e, sw;
	struct engine en;
	uint32_t seed = 1;
	int16_t mo[FRAME], so[FRAME];
	int i;

	/* The call: the far end talks from 0.5 s to the end, and from 6 to
	 * 8 s the owner talks too, 8 dB under the far end's echo */
	noise_fill(far + SEC / 2, TOTAL - SEC / 2, 8000, &seed);
	noise_fill(near + 6 * SEC, 2 * SEC, 1800, &seed);

	echo_init(&e, ECHO_AEC, 512);
	CHECK(e.mode == ECHO_AEC && e.aec);
	run(&e);

	/* 1. Before it has learned anything it is voice switching: the very
	 * first echo is already 30 dB down */
	CHECK(!adapted_at[(SEC / 2) / FRAME + 4]);
	CHECK(db(rms(sent + SEC / 2 + DELAY, 4 * FRAME), echo_rms(SEC / 2 + DELAY, 4 * FRAME)) < -30);

	/* 2. It learns the room within 3 s, and the far end alone still
	 * hears nothing of itself */
	CHECK(adapted_at[(3 * SEC + SEC / 2) / FRAME]);
	CHECK(state_at[(5 * SEC) / FRAME] == ECHO_RX);
	printf("  far end alone: echo %.1f dB down, the switch's share %.1f dB\n",
	       -db(rms(sent + 4 * SEC, 2 * SEC - FRAME), echo_rms(4 * SEC, 2 * SEC - FRAME)),
	       -db(e.mic_g, Q15_ONE));
	CHECK(db(rms(sent + 4 * SEC, 2 * SEC - FRAME), echo_rms(4 * SEC, 2 * SEC - FRAME)) < -30);

	/* 3. Both talking: the owner is heard at full level, although quieter
	 * than the echo, and the loudspeaker is only 6 dB down */
	CHECK(state_at[(7 * SEC) / FRAME] == ECHO_TX);
	{
		/* what is sent is some gain times the owner, plus what is
		 * left of the echo */
		const int16_t *o = sent + 6 * SEC + SEC / 2, *w = near + 6 * SEC + SEC / 2;
		double ow = 0, ww = 0, oo = 0, g, left, echo = echo_rms(6 * SEC + SEC / 2, SEC);

		for (i = 0; i < SEC; i++) {
			ow += (double)o[i] * w[i];
			ww += (double)w[i] * w[i];
			oo += (double)o[i] * o[i];
		}
		g = ow / ww;
		left = sqrt((oo - g * g * ww) / SEC);
		printf("  both talking: owner sent at %.1f dB, echo left %.1f dB down (the loudspeaker's 6 dB included)\n",
		       db(g, 1), -db(left, 2 * echo));
		CHECK_RANGE(db(g, 1), -1.5, 0.5);
		CHECK(db(left, 2 * echo) < -20);
		CHECK_RANGE(db(rms(played + 6 * SEC + SEC / 2, SEC), rms(far + 6 * SEC + SEC / 2, SEC)), -6.5, -5.5);
	}
	/* the owner breaks in within 60 ms */
	for (i = 6 * SEC / FRAME; state_at[i] != ECHO_TX; i++)
		;
	CHECK(i - 6 * SEC / FRAME <= 2);
	/* and afterwards the far end has the channel back, echo still gone */
	CHECK(state_at[(9 * SEC) / FRAME] == ECHO_RX);
	CHECK(db(rms(sent + 9 * SEC, SEC - FRAME), echo_rms(9 * SEC, SEC - FRAME)) < -30);

	/* 4. The same call through voice switching alone: the owner cannot
	 * break in. This is what the canceller is for. */
	echo_init(&sw, ECHO_SWITCH, 512);
	{
		static int16_t aec_sent[TOTAL];
		memcpy(aec_sent, sent, sizeof(sent));
		run(&sw);
		CHECK(state_at[(7 * SEC) / FRAME] == ECHO_RX);
		CHECK(db(rms(sent + 6 * SEC + SEC / 2, SEC), rms(near + 6 * SEC + SEC / 2, SEC)) < -20);
		CHECK(db(rms(aec_sent + 6 * SEC + SEC / 2, SEC), rms(sent + 6 * SEC + SEC / 2, SEC)) > 20);
	}
	echo_free(&e);
	CHECK(!e.aec);

	/* 5. Only the owner talks: passes, from the first frame */
	memset(far, 0, sizeof(far));
	memset(near, 0, sizeof(near));
	noise_fill(near + SEC, 2 * SEC, 6000, &seed);
	echo_init(&e, ECHO_AEC, 512);
	run(&e);
	CHECK_RANGE(db(rms(sent + SEC, 2 * SEC), rms(near + SEC, 2 * SEC)), -1.0, 0.5);
	CHECK_RANGE(db(rms(sent + SEC, FRAME), rms(near + SEC, FRAME)), -1.5, 0.5);
	echo_free(&e);

	/* 6. Background noise alone never switches anything */
	memset(near, 0, sizeof(near));
	echo_init(&e, ECHO_AEC, 512);
	run(&e);
	for (i = 0; i < TOTAL / FRAME; i++)
		if (state_at[i] != ECHO_IDLE)
			break;
	CHECK(i == TOTAL / FRAME);
	echo_free(&e);

	/* 7. Through the engine: a route set to "aec" gets a canceller, a
	 * route change gives it back, and coming back starts a fresh one */
	engine_init(&en);
	routes[T48AC_ROUTE_SPEAKER].echo = ECHO_AEC;
	engine_set_route(&en, T48AC_ROUTE_SPEAKER);
	CHECK(en.echo.mode == ECHO_AEC && en.echo.aec);
	for (i = 0; i < 50; i++)
		engine_tick(&en, far + i * FRAME, near + i * FRAME, mo, so);
	engine_set_route(&en, T48AC_ROUTE_HANDSET);
	CHECK(en.echo.mode == ECHO_NONE && !en.echo.aec);
	engine_set_route(&en, T48AC_ROUTE_SPEAKER);
	CHECK(en.echo.aec && !en.echo.adapted);
	engine_set_route(&en, T48AC_ROUTE_NONE);
	CHECK(!en.echo.aec);
	routes[T48AC_ROUTE_SPEAKER].echo = ECHO_SWITCH;

	T_DONE("aec");
}
