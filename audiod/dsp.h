/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48-audiod: the small integer sample helpers (issue #24). Everything on
 * the 20 ms path is 16 and 32 bit integer work: the phone's ARM11 has no
 * floating point unit worth using.
 */
#ifndef AUDIOD_DSP_H
#define AUDIOD_DSP_H
#include <stdint.h>

#define SRATE     16000
#define FRAME_MS  20
#define FRAME     (SRATE * FRAME_MS / 1000)	/* 320 samples */
#define Q15_ONE   32767

static inline int16_t sat16(int32_t v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

/* Volume level 0..15 as a Q15 gain: 0 is silent, 15 is full, 3 dB a step */
int dsp_vol_q15(int level);

/* x *= gain (Q15) */
void dsp_gain(int16_t *x, int n, int gain_q15);

/* out = a + b * gain (Q15), saturated. a may be the same buffer as out. */
void dsp_mix(int16_t *out, const int16_t *a, const int16_t *b, int gain_q15, int n);

/* Mean of |x| over the frame: the level measure used everywhere */
int dsp_level(const int16_t *x, int n);

/* Removes DC and rumble below about 20 Hz. The codec's preamp adds a large
 * DC offset above 18 dB (docs/audio.md). */
struct dcblock { int32_t mean_q8; };
void dsp_dcblock(struct dcblock *st, int16_t *x, int n);

#endif
