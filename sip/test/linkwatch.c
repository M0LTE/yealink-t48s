// SPDX-License-Identifier: AGPL-3.0-or-later
//
// t48keepalive's link decisions (sip/modules/t48keepalive/linkwatch.h, issue
// #77) on a fake clock: the time is a number passed in, nothing waits.
//
//   the first word about a link is only noted; a carrier coming back is acted
//   on at once; one sooner than 30 s after the last action is put off to 30 s
//   after it; a carrier going down again cancels what was put off; links are
//   told apart; a carrier staying up or down does nothing
//
// Build and run: sip/test/regbackoff-test.sh
#include <stdio.h>
#include "../modules/t48keepalive/linkwatch.h"

static int failures;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

int main(void)
{
	struct linkwatch lw = { 0 };
	uint64_t when = 0;

	check(linkwatch_event(&lw, 2, true, 1000, &when) == LW_NOTHING,
	      "first word about eth0 (up): noted only");
	check(linkwatch_event(&lw, 2, true, 2000, &when) == LW_NOTHING,
	      "still up: nothing");
	check(linkwatch_event(&lw, 2, false, 3000, &when) == LW_NOTHING,
	      "down: nothing to cancel");
	check(linkwatch_event(&lw, 2, true, 50000, &when) == LW_NOW,
	      "cable back: at once");

	/* a flap 10 s later: put off to 30 s after the last action */
	check(linkwatch_event(&lw, 2, false, 60000, &when) == LW_NOTHING &&
	      linkwatch_event(&lw, 2, true, 60500, &when) == LW_LATER &&
	      when == 80000, "back again 10.5 s later: put off to 30 s after the last");
	check(linkwatch_event(&lw, 2, false, 61000, &when) == LW_CANCEL,
	      "down again before then: cancelled");
	check(linkwatch_event(&lw, 2, true, 62000, &when) == LW_LATER &&
	      when == 80000, "and up again: put off to the same time");
	linkwatch_fired(&lw, 80000);
	check(linkwatch_event(&lw, 2, false, 90000, &when) == LW_NOTHING &&
	      linkwatch_event(&lw, 2, true, 95000, &when) == LW_LATER &&
	      when == 110000, "the put-off action counts as the last one");
	check(linkwatch_event(&lw, 2, false, 96000, &when) == LW_CANCEL &&
	      linkwatch_event(&lw, 2, true, 110000, &when) == LW_NOW,
	      "30 s after the last: at once");

	/* another link is its own; the damping is shared */
	check(linkwatch_event(&lw, 5, false, 120000, &when) == LW_NOTHING &&
	      linkwatch_event(&lw, 5, true, 150000, &when) == LW_NOW,
	      "a second link: noted, then acted on");

	if (failures)
		printf("%d failed\n", failures);
	return failures ? 1 : 0;
}
