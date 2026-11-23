// SPDX-License-Identifier: AGPL-3.0-or-later
/* Echo canceller comparison (issue #35): what one 20 ms frame costs.
 *   aeccpu [SECONDS] [CANDIDATE...]
 * Silent: it touches no audio device. Each candidate is fed the same made-up
 * call (far end talking in bursts, its echo 45 ms late with a decaying tail,
 * the owner talking over part of it, background noise) and every frame is
 * timed by the wall clock. The median is the figure when the phone is idle;
 * the quietest tenth is printed beside it, because frames interrupted by
 * something else (an SSH login takes half the processor for seconds) push
 * the median up and leave that alone. The kernel's per-process accounting is
 * no use here (docs/audiod.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cand.h"

static uint32_t seed = 12345;
static int rnd(void)
{
	seed = seed * 1664525u + 1013904223u;
	return (int16_t)(seed >> 16);
}

static long long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

int main(int argc, char **argv)
{
	static const int tails[] = { 64, 128, 200 };
	int secs = argc > 1 ? atoi(argv[1]) : 8, frames, n, c, t, i, f;
	int16_t *far, *mic, spk[FRAME], m[FRAME];
	int *us;
	int32_t lp = 0;

	if (secs < 2)
		secs = 2;
	frames = secs * 50;
	n = frames * FRAME;
	far = calloc(n, 2);
	mic = calloc(n, 2);
	us = calloc(frames, sizeof(int));

	/* far end: low-passed noise in bursts of 1.5 s every 2 s */
	for (i = 0; i < n; i++) {
		lp += (rnd() - lp) / 4;
		far[i] = (i % (2 * SRATE)) < 3 * SRATE / 2 ? (int16_t)(lp / 3) : 0;
	}
	/* microphone: the echo, some noise, and the owner in the third second of every four */
	for (i = 0; i < n; i++) {
		int32_t v = rnd() / 256, k, d = 45 * 16, g = 16384;
		for (k = 0; k < 8 && i >= d; k++, d += 37, g = g * 5 / 8)
			v += (far[i - d] * g) >> 15;
		if ((i % (4 * SRATE)) / SRATE == 2)
			v += rnd() / 8;
		mic[i] = sat16(v);
	}

	printf("%-12s %5s %9s %9s %9s %7s\n", "candidate", "tail", "10% us", "median us", "mean us", "CPU");
	for (c = 0; c < ncands; c++) {
		if (argc > 2) {
			for (i = 2; i < argc && strcmp(argv[i], cands[c].name); i++)
				;
			if (i == argc)
				continue;
		}
		for (t = 0; t < (cands[c].tails ? 3 : 1); t++) {
			long long sum = 0;
			unsigned sink = 0;

			if (cands[c].init(tails[t])) {
				printf("%-12s %5d failed to start\n", cands[c].name, tails[t]);
				continue;
			}
			for (f = 0; f < frames; f++) {
				long long t0;

				memcpy(spk, far + f * FRAME, sizeof(spk));
				memcpy(m, mic + f * FRAME, sizeof(m));
				t0 = now_ns();
				cands[c].run(spk, m);
				us[f] = (int)((now_ns() - t0) / 1000);
				sum += us[f];
				sink += (unsigned)m[7] + (unsigned)spk[9];
			}
			qsort(us, frames, sizeof(int), cmp);
			if (cands[c].tails)
				printf("%-12s %5d", cands[c].name, tails[t]);
			else
				printf("%-12s %5s", cands[c].name, "-");
			printf(" %9d %9d %9lld %6.1f%%   (%u)\n", us[frames / 10], us[frames / 2],
			       sum / frames, us[frames / 2] / 200.0, sink & 0xffff);
			fflush(stdout);
		}
	}
	return 0;
}
