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

/*
 * The loudspeaker's limit (issue #57). The amplifier runs from the phone's
 * 5 V supply, and a loud steady tone pulls it down until the processor
 * resets. Measured on the phone on 2026-11-24, with the gains in SPK_MAX_DAC
 * and SPK_MAX_OUT, the screen at full brightness and every lamp lit: the old
 * ringer survived a peak of 4020 (-18.2 dBFS) and reset the phone at 4510
 * (-17.2 dBFS), the same under Linux 3.6 and 6.18. It has also been reported
 * to reset at 2850 (-21.2 dBFS, ringer volume 10 as it was then). The limit
 * is 4 dB under the lowest of those and 7.5 dB under the measured one.
 * Nothing sent to the amplifier peaks above it, whatever the volume.
 */
#define SPK_LIMIT    1800	/* peak, in samples: -25.2 dBFS */
#define SPK_MAX_DAC  30		/* "Codec A Playback Volume": -6 dB */
#define SPK_MAX_OUT  6		/* "Codec A Output A Playback Volume": -3 dB */

/* The ringer volume 0..15 as a Q15 gain for the on-hook loudspeaker: 0 is
 * silent, 15 puts the ringer (peak 16000 in tone.c) exactly on SPK_LIMIT,
 * 2 dB a step. Tones and key sounds on hook use it too. */
int dsp_alert_q15(int level);

/* Peak limiter: no sample leaves above ceiling. A peak above it turns the
 * gain down at once, and the gain comes back over about 60 ms, so a steady
 * tone is turned down, not clipped. */
struct limiter { int32_t env; };
void dsp_limit(struct limiter *st, int16_t *x, int n, int ceiling);

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
