/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48-audiod: the queue of loudspeaker frames from baresip (issue #24).
 *
 * The service is the clock. Each tick it sends one microphone frame and
 * plays one loudspeaker frame; baresip answers each microphone frame with a
 * loudspeaker frame, normally within a millisecond, so the queue holds one
 * frame when the next tick comes. This queue is what happens otherwise:
 *  - nothing there (baresip was late): that tick plays silence;
 *  - a late frame then leaves two queued, 20 ms of extra delay. If the queue
 *    has held a spare frame for a whole second, one is dropped to get the
 *    delay back;
 *  - more than FQ_CAP queued (baresip sending faster than the clock, which
 *    should not happen): the oldest is dropped.
 */
#ifndef AUDIOD_PACE_H
#define AUDIOD_PACE_H
#include <stdint.h>
#include "dsp.h"

#define FQ_CAP        4
#define FQ_TRIM_TICKS 50	/* one second */

struct fq {
	int16_t frame[FQ_CAP][FRAME];
	int head, count;
	int spare_ticks;	/* ticks in a row with more than one frame waiting */
	uint32_t played, underruns, trimmed, overflowed;
};

void fq_reset(struct fq *q);
void fq_push(struct fq *q, const int16_t *frame);
/* One tick: 1 and a frame, or 0 (play silence) */
int fq_pop(struct fq *q, int16_t *frame);

#endif
