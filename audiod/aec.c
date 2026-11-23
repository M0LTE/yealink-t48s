// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: the echo canceller. See aec.h. */
#include <stdlib.h>
#include "aec.h"
#include "speex/speex_echo.h"

/* our addition to speexdsp's mdf.c (third_party/speexdsp/README.t48) */
void speex_echo_t48_status(const SpeexEchoState *st, int *adapted, int *leak_q15);

struct aec *aec_new(void)
{
	SpeexEchoState *st = speex_echo_state_init(FRAME, AEC_TAIL);
	int rate = SRATE;

	if (st)
		speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
	return (struct aec *)st;
}

void aec_free(struct aec *a)
{
	if (a)
		speex_echo_state_destroy((SpeexEchoState *)a);
}

void aec_cancel(struct aec *a, const int16_t *ref, const int16_t *mic, int16_t *out,
		int *adapted, int *leak_q15)
{
	speex_echo_cancellation((SpeexEchoState *)a, mic, ref, out);
	speex_echo_t48_status((SpeexEchoState *)a, adapted, leak_q15);
}
