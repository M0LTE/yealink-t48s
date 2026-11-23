/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48-audiod: the echo canceller itself (issue #35, docs/aec.md). speexdsp's
 * fixed-point canceller (third_party/speexdsp) behind three calls, so echo.c
 * does not see its insides. It learns how the loudspeaker's sound arrives at
 * the microphone over the first 140 ms and takes that much away.
 */
#ifndef AUDIOD_AEC_H
#define AUDIOD_AEC_H
#include <stdint.h>
#include "dsp.h"

#define AEC_TAIL (6 * FRAME)	/* 120 ms of echo path modelled */

struct aec;

struct aec *aec_new(void);	/* 0 if there is no memory for it */
void aec_free(struct aec *a);

/* One frame. ref: what the loudspeaker was given (the caller delays it; the
 * echo must not arrive before it). mic: as captured. out: the microphone with
 * the estimated echo removed. *adapted: 1 once it has learned the path.
 * *leak_q15: the share of the estimated echo's power it reckons is still in
 * out, 32767 = all. */
void aec_cancel(struct aec *a, const int16_t *ref, const int16_t *mic, int16_t *out,
		int *adapted, int *leak_q15);

#endif
