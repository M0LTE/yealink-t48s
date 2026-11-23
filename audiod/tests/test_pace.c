// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod test: frame pacing. The loudspeaker queue's behaviour when
 * baresip is on time, late or early, and the null backend's 20 ms clock. */
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "t.h"
#include "../pace.h"
#include "../hw.h"

static int16_t f[FRAME], out[FRAME];

static void push(struct fq *q, int tag)
{
	f[0] = (int16_t)tag;
	fq_push(q, f);
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
	struct fq q;
	struct hw *hw;
	struct pollfd p;
	double t0, t1;
	int i, n, r;

	/* before baresip has sent anything: silence, and not an error */
	fq_reset(&q);
	CHECK(fq_pop(&q, out) == 0 && q.underruns == 0);

	/* the normal case: one frame arrives between ticks, in order */
	for (i = 1; i <= 100; i++) {
		push(&q, i);
		CHECK(fq_pop(&q, out) == 1 && out[0] == i);
	}
	CHECK(q.played == 100 && q.underruns == 0 && q.trimmed == 0 && q.overflowed == 0);

	/* baresip misses a tick: that tick is silent and counted */
	CHECK(fq_pop(&q, out) == 0 && q.underruns == 1);
	/* its frame then arrives late along with the next: both get played,
	 * in order, 20 ms behind */
	push(&q, 101);
	push(&q, 102);
	CHECK(fq_pop(&q, out) == 1 && out[0] == 101);
	for (i = 103; i < 103 + FQ_TRIM_TICKS - 2; i++) {
		push(&q, i);
		CHECK(fq_pop(&q, out) == 1 && out[0] == i - 1);
	}
	CHECK(q.trimmed == 0);
	/* after a second of running one behind, one frame is dropped and the
	 * delay is back to nothing */
	push(&q, 900);
	CHECK(fq_pop(&q, out) == 1 && out[0] == 900 && q.trimmed == 1 && q.count == 0);
	push(&q, 901);
	CHECK(fq_pop(&q, out) == 1 && out[0] == 901);

	/* a short burst of lateness does not trigger the drop */
	fq_reset(&q);
	push(&q, 1); push(&q, 2);
	for (i = 3; i < 20; i++) {
		CHECK(fq_pop(&q, out) == 1);
		push(&q, i);
	}
	CHECK(fq_pop(&q, out) == 1);	/* queue back to one */
	CHECK(fq_pop(&q, out) == 1 && q.count == 0 && q.trimmed == 0);

	/* baresip sending faster than the clock: the oldest go, the newest stay */
	fq_reset(&q);
	for (i = 1; i <= FQ_CAP + 3; i++)
		push(&q, i);
	CHECK(q.overflowed == 3 && q.count == FQ_CAP);
	CHECK(fq_pop(&q, out) == 1 && out[0] == 4);

	/* the null backend's clock: 50 frames a second, no more, no fewer */
	hw = hw_null_new(0, 0, 0, 0);
	CHECK(hw && hw->tick_fd(hw) == -1 && hw->capture(hw, out) == -1);
	CHECK(hw->start(hw, &routes[1]) == 0);
	p.fd = hw->tick_fd(hw);
	p.events = POLLIN;
	t0 = now();
	for (n = 0; n < 50;) {
		CHECK(poll(&p, 1, 1000) == 1);
		while ((r = hw->capture(hw, out)) == 1)
			n++;
		CHECK(r == 0);
	}
	t1 = now();
	CHECK_RANGE(t1 - t0, 0.99, 1.03);
	/* a route change keeps the same clock */
	CHECK(hw->start(hw, &routes[2]) == 0 && hw->tick_fd(hw) == p.fd);

	/* a short stall is caught up frame for frame (nothing lost) */
	usleep(70000);
	for (n = 0; hw->capture(hw, out) == 1;)
		n++;
	CHECK_RANGE(n, 3, 4);
	/* a long one is not replayed: one frame, counted as an overrun */
	usleep(300000);
	for (n = 0; hw->capture(hw, out) == 1;)
		n++;
	CHECK(n == 1 && hw->cap_xruns == 1);

	/* stopped: no clock at all */
	hw->stop(hw);
	CHECK(hw->tick_fd(hw) == -1);
	hw->destroy(hw);

	T_DONE("pace");
}
