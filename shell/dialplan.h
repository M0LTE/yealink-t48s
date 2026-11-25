// SPDX-License-Identifier: AGPL-3.0-or-later
/* The dial plan: when a number being typed is complete (issue #77, docs/phone.md#dial-plan). */
#ifndef DIALPLAN_H
#define DIALPLAN_H

/* UK numbers that are always the same length dial at once; anything else 4 s after the last digit */
#define DP_DEFAULT "999|112|101|105|111|116xxx|118xxx|0[1237]xxxxxxxxx|05[56]xxxxxxxx|08[47]xxxxxxxx|0808xxxxxxx|" \
                   "09xxxxxxxxx|[*#][*#x][*#x].T|+xx.T|x.T"
#define DP_TIMER_MS 4000   /* T with no number after it */
#define DP_MAX 160         /* the longest map: what the phone's keyboard takes */

enum dp_result {
    DP_NONE,       /* no pattern can match, whatever comes next */
    DP_PARTIAL,    /* more digits could make a match; nothing to do yet */
    DP_TIMER,      /* dial if nothing more is typed within *ms */
    DP_NOW,        /* complete: dial now */
};

/* 0 if map is a digit map this file understands; -1 if not, with where it went wrong in *at (may be NULL) */
int dp_check(const char *map, int *at);
/* what to do with number as it stands; *ms is set for DP_TIMER. A map that does not check is DP_NONE. */
enum dp_result dp_match(const char *map, const char *number, int *ms);

#endif
