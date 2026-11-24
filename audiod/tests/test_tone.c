// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod test: the tones are the right pitch, the right rhythm, and do
 * not click. */
#include <string.h>
#include "t.h"
#include "../dsp.h"
#include "../tone.h"

#define SEC SRATE
static int16_t buf[8 * SEC];

/* Render in 20 ms frames, as the daemon does */
static int render(struct tone_gen *g, int16_t *out, int n)
{
	int i, any = 0;
	for (i = 0; i + FRAME <= n; i += FRAME)
		any |= tone_render(g, out + i, FRAME);
	return any;
}

/* 1 if the 5 ms starting at sample at holds sound */
static int sounding(const int16_t *x, int at)
{
	return rms(x + at, 80) > 100;
}

int main(void)
{
	struct tone_gen g;
	const char *keys = "0123456789*#";
	static const int lo[12] = { 941, 697, 697, 697, 770, 770, 770, 852, 852, 852, 941, 941 };
	static const int hi[12] = { 1336, 1209, 1336, 1477, 1209, 1336, 1477, 1209, 1336, 1477, 1209, 1477 };
	int i, k;

	/* nothing started: silence, and it says so */
	memset(&g, 0, sizeof(g));
	buf[5] = 99;
	CHECK(tone_render(&g, buf, FRAME) == 0 && buf[5] == 0);

	/* dial tone: 350 + 440 Hz, continuous */
	tone_start(&g, &tone_dial);
	render(&g, buf, 2 * SEC);
	CHECK_RANGE(tone_amp(buf + SEC, SEC, 350), 4950, 5050);
	CHECK_RANGE(tone_amp(buf + SEC, SEC, 440), 4950, 5050);
	CHECK(tone_amp(buf + SEC, SEC, 400) < 30);
	CHECK(tone_active(&g));
	/* nothing but the two tones: the sine table is clean */
	CHECK_RANGE(rms(buf + SEC, SEC), 4990, 5010);
	/* starts without a step */
	CHECK(max_step(buf, 2 * SEC) < 1700);
	CHECK(abs(buf[0]) < 100);

	/* stopping fades out within 2 ms and then reports silence */
	tone_stop(&g);
	CHECK(tone_render(&g, buf, FRAME) == 1);
	CHECK(max_step(buf, FRAME) < 1700);
	CHECK(rms(buf + 40, FRAME - 40) == 0);
	CHECK(!tone_active(&g));
	CHECK(tone_render(&g, buf, FRAME) == 0);

	/* busy: 400 Hz, 375 ms on, 375 ms off, for ever */
	tone_start(&g, &tone_busy);
	render(&g, buf, 3 * SEC);
	CHECK_RANGE(tone_amp(buf + 800, 4800, 400), 7900, 8050);
	for (k = 0; k < 3; k++) {
		int t0 = k * 750 * 16;
		CHECK(sounding(buf, t0 + 160) && sounding(buf, t0 + 370 * 16 - 80));
		CHECK(!sounding(buf, t0 + 376 * 16) && !sounding(buf, t0 + 745 * 16 - 80));
	}
	CHECK(max_step(buf, 3 * SEC) < 1400);	/* on and off without a click */

	/* ringback: 400 + 450 Hz, 0.4 on, 0.2 off, 0.4 on, 2 off */
	tone_start(&g, &tone_ringback);
	render(&g, buf, 7 * SEC);
	CHECK_RANGE(tone_amp(buf + 800, 4800, 400), 4900, 5100);
	CHECK_RANGE(tone_amp(buf + 800, 4800, 450), 4900, 5100);
	for (k = 0; k < 2; k++) {
		int t0 = k * 3 * SEC;
		CHECK(sounding(buf, t0 + 200 * 16) && !sounding(buf, t0 + 500 * 16));
		CHECK(sounding(buf, t0 + 800 * 16) && !sounding(buf, t0 + 1100 * 16));
		CHECK(!sounding(buf, t0 + 2900 * 16));
	}

	/* error: steady 400 Hz. Call waiting: a 100 ms beep every 3.1 s */
	tone_start(&g, &tone_error);
	render(&g, buf, SEC);
	CHECK_RANGE(tone_amp(buf, SEC, 400), 7950, 8050);
	tone_start(&g, &tone_waiting);
	render(&g, buf, 4 * SEC);
	CHECK(sounding(buf, 50 * 16) && !sounding(buf, 110 * 16) && !sounding(buf, 3000 * 16));
	CHECK(sounding(buf, 3150 * 16));

	/* ringer: two sine notes taking turns 20 times a second, ring rhythm.
	 * Each note holds for 19 ms and fades into the next over 6 ms. */
	tone_start(&g, &tone_ring);
	render(&g, buf, 7 * SEC);
	CHECK(tone_amp(buf + 416, 256, 1250) > 14000);	/* second note, held */
	CHECK(tone_amp(buf + 816, 256, 1000) > 14000);	/* first note again */
	CHECK(tone_amp(buf + 416, 256, 1250) > 4 * tone_amp(buf + 416, 256, 1000));
	CHECK(tone_amp(buf + 816, 256, 1000) > 4 * tone_amp(buf + 816, 256, 1250));
	CHECK(sounding(buf, 200 * 16) && !sounding(buf, 500 * 16) && sounding(buf, 800 * 16));
	CHECK(!sounding(buf, 1500 * 16) && sounding(buf, 3200 * 16));
	CHECK(tone_active(&g));
	/* gentle on the amplifier (issue #57). Never above one note's level,
	 * also while two notes overlap: */
	for (i = 0, k = 0; i < 7 * SEC; i++)
		if (abs(buf[i]) > k)
			k = abs(buf[i]);
	CHECK(k <= 16000 && k > 15500);
	/* each burst fades in over 20 ms and out again: quarter by quarter
	 * the level rises, and the first and last 2.5 ms are almost nothing */
	for (k = 0; k < 2; k++) {
		const int16_t *r = buf + k * 600 * 16;	/* the two bursts of a ring */
		CHECK(rms(r, 40) < 300 && rms(r + 6400 - 40, 40) < 300);
		CHECK(rms(r, 80) < rms(r + 80, 80) && rms(r + 80, 80) < rms(r + 160, 80));
		CHECK(rms(r + 160, 80) < rms(r + 240, 80) && rms(r + 240, 80) < 16000 * 0.71);
		CHECK(rms(r + 6320, 80) < rms(r + 6240, 80) && rms(r + 6240, 80) < rms(r + 6160, 80));
		CHECK_RANGE(rms(r + 320, 5760), 16000 * 0.66, 16000 * 0.71);	/* as loud as a sine all through */
	}
	/* half way through a change of note both notes are there, equally */
	CHECK_RANGE(tone_amp(buf + 752 - 16, 32, 1000) + tone_amp(buf + 752 - 16, 32, 1250), 12000, 20000);
	CHECK_RANGE(rms(buf + 704, 96), 16000 * 0.5, 16000 * 0.71);
	/* only sine waves: nothing to speak of away from the two notes, and
	 * the wave never turns a corner (a 1250 Hz sine at this level bends
	 * by 3780 a sample at most) */
	for (i = 1750; i <= 7750; i += 250)
		CHECK(tone_amp(buf, 6400, i) < 40);
	for (i = 2, k = 0; i < 7 * SEC; i++)
		if (abs(buf[i] - 2 * buf[i - 1] + buf[i - 2]) > k)
			k = abs(buf[i] - 2 * buf[i - 1] + buf[i - 2]);
	CHECK(k < 3900);
	/* stopped in mid ring: a 20 ms fade, not a cut */
	tone_start(&g, &tone_ring);
	render(&g, buf, 3200);
	tone_stop(&g);
	render(&g, buf, 640);
	CHECK(rms(buf, 80) > 8000 && rms(buf + 240, 80) < rms(buf, 80) && rms(buf + 320, 320) == 0);
	CHECK(!tone_active(&g));

	/* keys: the DTMF pair for 80 ms, then silence, without being stopped */
	for (k = 0; k < 12; k++) {
		tone_start(&g, tone_for_key(keys[k]));
		CHECK(render(&g, buf, SEC / 2) == 1);
		CHECK_RANGE(tone_amp(buf + 160, 960, lo[k]), 4500, 5300);
		CHECK_RANGE(tone_amp(buf + 160, 960, hi[k]), 4500, 5300);
		for (i = 0; i < 12; i++)	/* and no other row or column */
			if (lo[i] != lo[k])
				CHECK(tone_amp(buf + 160, 960, lo[i]) < 900);
		CHECK(sounding(buf, 70 * 16) && !sounding(buf, 81 * 16));
		CHECK(!tone_active(&g));
		CHECK(max_step(buf, SEC / 2) < 4700);
	}
	/* any other key: a click, over in 10 ms */
	tone_start(&g, tone_for_key('x'));
	render(&g, buf, SEC / 4);
	CHECK(rms(buf, 128) > 500 && rms(buf + 160, SEC / 4 - 160) == 0 && !tone_active(&g));

	/* never louder than its table says (two tones at 5000: peak 10000) */
	tone_start(&g, &tone_dial);
	render(&g, buf, SEC);
	for (i = 0, k = 0; i < SEC; i++)
		if (abs(buf[i]) > k)
			k = abs(buf[i]);
	CHECK(k <= 10000 && k > 9000);

	T_DONE("tone");
}
