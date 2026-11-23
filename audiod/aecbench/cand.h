/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Echo canceller comparison (issue #35): one face for every candidate, so the
 * CPU bench on the phone and the quality bench on a PC run the same code.
 */
#ifndef AECBENCH_CAND_H
#define AECBENCH_CAND_H
#include <stdint.h>
#include "../dsp.h"

struct cand {
	const char *name;
	const char *what;
	int tails;	/* 1: the tail length means something to it */
	/* 0 if ready. tail_ms: how much echo path to model. */
	int (*init)(int tail_ms);
	/* One 20 ms frame, in place, as echo_process: spk is about to be
	 * played, mic was just captured. */
	void (*run)(int16_t *spk, int16_t *mic);
};

extern const struct cand cands[];
extern const int ncands;
const struct cand *cand_find(const char *name);

#endif
