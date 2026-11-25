/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48-audiod: tones made in software (issue #24): call progress (dial,
 * ringback, busy, error, call waiting), the ringer, key clicks and the DTMF
 * feedback sounds. A tone is a list of segments; the generator is a phase
 * accumulator reading a 256-step sine table, integer only.
 *
 * Nothing here is anything but sine waves, and every start, stop and change
 * of note is a smooth fade: the loudspeaker's amplifier can pull the phone's
 * supply down (issue #57), and edges are what it finds hardest.
 */
#ifndef AUDIOD_TONE_H
#define AUDIOD_TONE_H
#include <stdint.h>

struct tone_seg {
	uint16_t f1, f2;	/* Hz. Both 0: silence. f2 0: one tone. */
	uint16_t ms;		/* how long; 0 = until stopped */
	int16_t amp;		/* peak of each tone, full scale 32767 */
	uint8_t warble_ms;	/* 0: f1 and f2 sound together. Otherwise they
				 * take turns this often, one fading into the
				 * other (the ringer) */
	uint8_t fade_ms;	/* fade in and out; 0 means 2 ms */
};

struct tone_def {
	const char *name;
	const struct tone_seg *seg;
	uint8_t nseg;
	uint8_t loop;		/* 1: start again after the last segment */
	/* A recorded sound in place of the segments (a custom ringtone):
	 * pcm_len samples, then PCM_GAP_MS of silence, round again. It fades
	 * in and out like a tone; its own start and end should be quiet. */
	const int16_t *pcm;
	uint32_t pcm_len;
};
#define PCM_GAP_MS 1000

struct tone_gen {
	const struct tone_def *def;	/* NULL: silent */
	uint8_t seg;
	uint8_t stopping;	/* fading out after tone_stop */
	uint32_t left;		/* samples left in this segment (if it is timed) */
	uint32_t ph1, ph2, inc1, inc2;
	uint32_t pcm_pos;	/* a recorded sound: the next sample, past
				 * pcm_len while in the gap */
	uint32_t warble_left, warble_len;
	uint8_t warble_second;
	int32_t env;		/* 0..32767, slewed over the fade time and then
				 * shaped into half a cosine, so nothing clicks */
	int32_t env_step;
};

/* The UK tone plan (the phone's first home). Another country is another
 * table of the same shape. */
extern const struct tone_def tone_dial, tone_ringback, tone_busy, tone_error,
	tone_waiting, tone_ring, tone_click;

/* The built-in ringtones, by t48ac_ringtone (tone_ring is the first) */
extern const struct tone_def *const tone_ringtones[];
extern const int tone_nringtones;

/* The sound for a key: DTMF for 0-9 * #, a click for anything else */
const struct tone_def *tone_for_key(int key);

void tone_start(struct tone_gen *g, const struct tone_def *def);
void tone_stop(struct tone_gen *g);		/* fades out over the segment's fade time */
static inline int tone_active(const struct tone_gen *g) { return g->def != 0; }

/* Write n samples. Returns 0 if the generator was and stays silent (out is
 * then all zeros), 1 otherwise. */
int tone_render(struct tone_gen *g, int16_t *out, int n);

#endif
