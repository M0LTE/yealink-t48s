/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * linkwatch: what t48keepalive does with a link's carrier (issue #77), apart
 * from netlink and timers so it can be tested on a fake clock
 * (sip/test/linkwatch.c). The caller says what each link is doing and when;
 * this says whether to act on a cable put back, and when.
 *
 * A link's carrier coming back up is acted on, at most once in DAMP_MS: an up
 * edge sooner than that after the last action is put off until DAMP_MS has
 * passed, so a flapping cable gives one registration and one DHCP request,
 * made once it has had time to settle. A carrier that goes down again
 * cancels what was put off. The first word about a link is only noted.
 */
#ifndef T48_LINKWATCH_H
#define T48_LINKWATCH_H

#include <stdbool.h>
#include <stdint.h>

enum {
	LINKWATCH_LINKS = 8,
	LINKWATCH_DAMP_MS = 30000,
};

enum linkwatch_do {
	LW_NOTHING,
	LW_NOW,           /* act now */
	LW_LATER,         /* act at *when (the caller's timer); replaces any before */
	LW_CANCEL,        /* drop what was put off */
};

struct linkwatch {
	struct { int index; bool up; } link[LINKWATCH_LINKS];
	unsigned n;
	bool acted;            /* acted at least once */
	uint64_t last;         /* when, in ms */
	bool pending;          /* something put off */
};

static inline enum linkwatch_do linkwatch_event(struct linkwatch *lw, int index,
						 bool up, uint64_t now,
						 uint64_t *when)
{
	unsigned i;
	bool was;

	for (i = 0; i < lw->n && lw->link[i].index != index; i++)
		;
	if (i == lw->n) {
		if (lw->n < LINKWATCH_LINKS) {
			lw->link[i].index = index;
			lw->link[i].up = up;
			lw->n++;
		}
		return LW_NOTHING;
	}
	was = lw->link[i].up;
	lw->link[i].up = up;
	if (was == up)
		return LW_NOTHING;
	if (!up) {
		if (!lw->pending)
			return LW_NOTHING;
		lw->pending = false;
		return LW_CANCEL;
	}
	if (!lw->acted || now - lw->last >= LINKWATCH_DAMP_MS) {
		lw->acted = true;
		lw->last = now;
		lw->pending = false;
		return LW_NOW;
	}
	lw->pending = true;
	*when = lw->last + LINKWATCH_DAMP_MS;
	return LW_LATER;
}

/* the caller's timer for LW_LATER has fired: act now */
static inline void linkwatch_fired(struct linkwatch *lw, uint64_t now)
{
	lw->pending = false;
	lw->acted = true;
	lw->last = now;
}

#endif
