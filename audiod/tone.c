// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: tone generation. See tone.h. */
#include <string.h>
#include "dsp.h"
#include "tone.h"

/* round(32767 * sin(2 pi i / 256)), i = 0..256 */
static const int16_t sine[257] = {
	0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
	9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
	18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811,
	25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
	30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
	32609, 32678, 32728, 32757, 32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
	32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571, 30273, 29956, 29621, 29268,
	28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
	23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151,
	15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179,
	6393, 5602, 4808, 4011, 3212, 2410, 1608, 804, 0, -804, -1608, -2410,
	-3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
	-12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
	-20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
	-27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
	-31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
	-32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580,
	-31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
	-27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403,
	-20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
	-12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011,
	-3212, -2410, -1608, -804, 0,
};

#define FADE_MS   2			/* fade in and out, unless the segment says */
#define XFADE     96			/* samples: 6 ms from one ringer note to the other */

/* Levels: a single tone at 8000 is -12 dBFS, two at 5000 each peak at -10 */
#define A2 5000
#define A1 8000

static const struct tone_seg seg_dial[]     = { { 350, 440, 0, A2, 0, 0 } };
static const struct tone_seg seg_ringback[] = { { 400, 450, 400, A2, 0, 0 }, { 0, 0, 200, 0, 0, 0 },
						{ 400, 450, 400, A2, 0, 0 }, { 0, 0, 2000, 0, 0, 0 } };
static const struct tone_seg seg_busy[]     = { { 400, 0, 375, A1, 0, 0 }, { 0, 0, 375, 0, 0, 0 } };
static const struct tone_seg seg_error[]    = { { 400, 0, 0, A1, 0, 0 } };
static const struct tone_seg seg_waiting[]  = { { 400, 0, 100, A1, 0, 0 }, { 0, 0, 3000, 0, 0, 0 } };
/* The ringer: two notes taking turns 20 times a second, in the UK ring
 * rhythm. Each note is a sine that fades into the other over 6 ms, and each
 * burst fades in and out over 20 ms (issue #57). 16000 is the level the
 * ringer volume is worked out from (dsp_alert_q15). */
static const struct tone_seg seg_ring[]     = { { 1000, 1250, 400, 16000, 25, 20 }, { 0, 0, 200, 0, 0, 0 },
						{ 1000, 1250, 400, 16000, 25, 20 }, { 0, 0, 2000, 0, 0, 0 } };
static const struct tone_seg seg_click[]    = { { 2000, 0, 8, 4000, 0, 0 } };

const struct tone_def tone_dial     = { "dial", seg_dial, 1, 1 };
const struct tone_def tone_ringback = { "ringback", seg_ringback, 4, 1 };
const struct tone_def tone_busy     = { "busy", seg_busy, 2, 1 };
const struct tone_def tone_error    = { "error", seg_error, 1, 1 };
const struct tone_def tone_waiting  = { "waiting", seg_waiting, 2, 1 };
const struct tone_def tone_ring     = { "ring", seg_ring, 4, 1 };
const struct tone_def tone_click    = { "click", seg_click, 1, 0 };

#define DTMF(lo, hi) { { lo, hi, 80, A2, 0, 0 } }
static const struct tone_seg seg_dtmf[12][1] = {
	DTMF(941, 1336),						/* 0 */
	DTMF(697, 1209), DTMF(697, 1336), DTMF(697, 1477),	/* 1 2 3 */
	DTMF(770, 1209), DTMF(770, 1336), DTMF(770, 1477),	/* 4 5 6 */
	DTMF(852, 1209), DTMF(852, 1336), DTMF(852, 1477),	/* 7 8 9 */
	DTMF(941, 1209), DTMF(941, 1477),			/* * # */
};
static const struct tone_def def_dtmf[12] = {
	{ "0", seg_dtmf[0], 1, 0 }, { "1", seg_dtmf[1], 1, 0 }, { "2", seg_dtmf[2], 1, 0 },
	{ "3", seg_dtmf[3], 1, 0 }, { "4", seg_dtmf[4], 1, 0 }, { "5", seg_dtmf[5], 1, 0 },
	{ "6", seg_dtmf[6], 1, 0 }, { "7", seg_dtmf[7], 1, 0 }, { "8", seg_dtmf[8], 1, 0 },
	{ "9", seg_dtmf[9], 1, 0 }, { "*", seg_dtmf[10], 1, 0 }, { "#", seg_dtmf[11], 1, 0 },
};

const struct tone_def *tone_for_key(int key)
{
	if (key >= '0' && key <= '9')
		return &def_dtmf[key - '0'];
	if (key == '*')
		return &def_dtmf[10];
	if (key == '#')
		return &def_dtmf[11];
	return &tone_click;
}

static uint32_t hz_to_inc(unsigned hz)
{
	return (uint32_t)(((uint64_t)hz << 32) / SRATE);
}

static inline int32_t sine_at(uint32_t ph)
{
	unsigned i = ph >> 24, frac = (ph >> 16) & 0xff;
	return sine[i] + (((sine[i + 1] - sine[i]) * (int32_t)frac) >> 8);
}

/* Half a cosine from 0 to 32767 as x goes from 0 to 32767: a fade with no
 * corner at either end */
static inline int32_t smooth(int32_t x)
{
	return (Q15_ONE - sine_at(((uint32_t)x << 16) + 0x40000000u)) >> 1;
}

static uint32_t fade_len(const struct tone_seg *s)
{
	return (uint32_t)(s->fade_ms ? s->fade_ms : FADE_MS) * (SRATE / 1000);
}

static void enter_seg(struct tone_gen *g, unsigned idx)
{
	const struct tone_seg *s = &g->def->seg[idx];

	g->seg = (uint8_t)idx;
	g->left = (uint32_t)s->ms * (SRATE / 1000);
	g->inc1 = hz_to_inc(s->f1);
	g->inc2 = hz_to_inc(s->f2);
	g->warble_len = (uint32_t)s->warble_ms * (SRATE / 1000);
	g->warble_left = g->warble_len;
	g->warble_second = 0;
	g->env_step = (int32_t)(32768 / fade_len(s));
}

void tone_start(struct tone_gen *g, const struct tone_def *def)
{
	memset(g, 0, sizeof(*g));
	if (!def || !def->nseg)
		return;
	g->def = def;
	enter_seg(g, 0);
}

void tone_stop(struct tone_gen *g)
{
	if (g->def)
		g->stopping = 1;
}

int tone_render(struct tone_gen *g, int16_t *out, int n)
{
	int i;

	if (!g->def) {
		memset(out, 0, (size_t)n * sizeof(*out));
		return 0;
	}
	for (i = 0; i < n; i++) {
		const struct tone_seg *s;
		int32_t v, target;

		if (!g->def) {
			out[i] = 0;
			continue;
		}
		s = &g->def->seg[g->seg];
		/* fade out before the end of a timed segment, and when stopped */
		target = (s->f1 && !g->stopping && (!s->ms || g->left > fade_len(s))) ? Q15_ONE : 0;
		if (g->env < target)
			g->env = g->env + g->env_step > target ? target : g->env + g->env_step;
		else if (g->env > target)
			g->env = g->env - g->env_step < target ? target : g->env - g->env_step;

		if (!g->env) {
			v = 0;
		} else if (g->warble_len) {
			/* Both notes run all the time; only their shares
			 * change. The shares add up to one, so the sum never
			 * peaks above one note alone. */
			int32_t now = sine_at(g->warble_second ? g->ph2 : g->ph1);
			int32_t next = sine_at(g->warble_second ? g->ph1 : g->ph2);

			v = now;
			if (g->warble_left <= XFADE && g->warble_len > XFADE) {
				int32_t w = smooth((int32_t)((XFADE - g->warble_left) * Q15_ONE / XFADE));
				v = (now * (Q15_ONE - w) + next * w) >> 15;
			}
			g->ph1 += g->inc1;
			g->ph2 += g->inc2;
			if (!--g->warble_left) {
				g->warble_left = g->warble_len;
				g->warble_second ^= 1;
			}
		} else {
			v = sine_at(g->ph1);
			g->ph1 += g->inc1;
			if (g->inc2) {
				v += sine_at(g->ph2);
				g->ph2 += g->inc2;
			}
		}
		v = (v * s->amp) >> 15;
		out[i] = sat16((v * smooth(g->env)) >> 15);

		if (g->stopping) {
			if (!g->env)
				g->def = 0;
			continue;
		}
		if (s->ms && !--g->left) {
			unsigned next = g->seg + 1u;
			if (next >= g->def->nseg) {
				if (!g->def->loop) {
					g->def = 0;
					continue;
				}
				next = 0;
			}
			g->env = 0;
			g->ph1 = g->ph2 = 0;
			enter_seg(g, next);
		}
	}
	return 1;
}
