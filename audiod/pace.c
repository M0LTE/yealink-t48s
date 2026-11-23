// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: loudspeaker frame queue. See pace.h. */
#include <string.h>
#include "pace.h"

void fq_reset(struct fq *q)
{
	memset(q, 0, sizeof(*q));
}

static void drop_oldest(struct fq *q)
{
	q->head = (q->head + 1) % FQ_CAP;
	q->count--;
}

void fq_push(struct fq *q, const int16_t *frame)
{
	if (q->count == FQ_CAP) {
		drop_oldest(q);
		q->overflowed++;
	}
	memcpy(q->frame[(q->head + q->count) % FQ_CAP], frame, sizeof(q->frame[0]));
	q->count++;
}

int fq_pop(struct fq *q, int16_t *frame)
{
	if (!q->count) {
		/* Not counted before the first frame ever arrives */
		if (q->played)
			q->underruns++;
		q->spare_ticks = 0;
		return 0;
	}
	if (q->count > 1) {
		if (++q->spare_ticks >= FQ_TRIM_TICKS) {
			drop_oldest(q);
			q->trimmed++;
			q->spare_ticks = 0;
		}
	} else {
		q->spare_ticks = 0;
	}
	memcpy(frame, q->frame[q->head], sizeof(q->frame[0]));
	drop_oldest(q);
	q->played++;
	return 1;
}
