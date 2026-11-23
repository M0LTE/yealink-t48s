// SPDX-License-Identifier: AGPL-3.0-or-later
/* Echo canceller comparison (issue #35): the candidates. See cand.h. */
#include <string.h>
#include "cand.h"
#include "../echo.h"
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#include "webrtc/modules/audio_processing/aecm/include/echo_control_mobile.h"

/* --- nothing, and today's voice switching -------------------------------- */

static int none_init(int tail_ms) { (void)tail_ms; return 0; }
static void none_run(int16_t *spk, int16_t *mic) { (void)spk; (void)mic; }

static struct echo sw;
static int switch_init(int tail_ms)
{
	(void)tail_ms;
	echo_init(&sw, ECHO_SWITCH, 512);	/* the speaker route's setting */
	return 0;
}
static void switch_run(int16_t *spk, int16_t *mic) { echo_process(&sw, spk, mic, FRAME); }

/* --- what audiod's "echo aec" does: speexdsp's canceller, 120 ms, then the
 * switch made shallow by it (../echo.c) ---------------------------------- */

static struct echo ae;
static int ae_used;
static int t48aec_init(int tail_ms)
{
	(void)tail_ms;
	if (ae_used)
		echo_free(&ae);
	ae_used = 1;
	echo_init(&ae, ECHO_AEC, 512);
	return ae.mode != ECHO_AEC;
}
static void t48aec_run(int16_t *spk, int16_t *mic) { echo_process(&ae, spk, mic, FRAME); }

/* --- speexdsp: MDF canceller, optionally with its preprocessor ----------- */

static SpeexEchoState *sx;
static SpeexPreprocessState *sxp;
static int sx_frame;

static int speex_setup(int tail_ms, int frame, int pp)
{
	int rate = SRATE, on = 1;

	if (sx)
		speex_echo_state_destroy(sx);
	if (sxp)
		speex_preprocess_state_destroy(sxp);
	sxp = 0;
	sx_frame = frame;
	sx = speex_echo_state_init(frame, tail_ms * SRATE / 1000);
	if (!sx)
		return -1;
	speex_echo_ctl(sx, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
	if (pp) {
		sxp = speex_preprocess_state_init(frame, SRATE);
		speex_preprocess_ctl(sxp, SPEEX_PREPROCESS_SET_DENOISE, &on);
		speex_preprocess_ctl(sxp, SPEEX_PREPROCESS_SET_ECHO_STATE, sx);
	}
	return 0;
}
static void speex_run(int16_t *spk, int16_t *mic)
{
	int16_t out[FRAME];
	int i;

	for (i = 0; i < FRAME; i += sx_frame) {
		speex_echo_cancellation(sx, mic + i, spk + i, out + i);
		if (sxp)
			speex_preprocess_run(sxp, out + i);
	}
	memcpy(mic, out, sizeof(out));
}
static int speex_init(int t) { return speex_setup(t, FRAME, 0); }
static int speexpp_init(int t) { return speex_setup(t, FRAME, 1); }
static int speex10_init(int t) { return speex_setup(t, FRAME / 2, 0); }
static int speex10pp_init(int t) { return speex_setup(t, FRAME / 2, 1); }

/* --- WebRTC AECM --------------------------------------------------------- */

static void *am;
static int am_delay_ms = 40;	/* what the sound card holds; it estimates the rest */

static int aecm_setup(int mode)
{
	AecmConfig cfg = { .cngMode = AecmTrue, .echoMode = (int16_t)mode };

	if (am)
		WebRtcAecm_Free(am);
	am = WebRtcAecm_Create();
	if (!am || WebRtcAecm_Init(am, SRATE))
		return -1;
	return WebRtcAecm_set_config(am, cfg);
}
static void aecm_run(int16_t *spk, int16_t *mic)
{
	int16_t out[FRAME];
	int i;

	/* it works in 10 ms pieces */
	for (i = 0; i < FRAME; i += 160) {
		WebRtcAecm_BufferFarend(am, spk + i, 160);
		WebRtcAecm_Process(am, mic + i, 0, out + i, 160, (int16_t)am_delay_ms);
	}
	memcpy(mic, out, sizeof(out));
}
static int aecm3_init(int t) { (void)t; return aecm_setup(3); }
static int aecm4_init(int t) { (void)t; return aecm_setup(4); }

/* --- the textbook: NLMS, one tap per sample of tail ----------------------- */
/* Here for its cost. It has no double-talk control at all, so the owner's
 * voice knocks it off course; a usable one would cost more, not less. */

#define NL_MAX 3200
static int32_t nl_w[NL_MAX];		/* Q22 */
static int16_t nl_w16[NL_MAX];		/* Q14, what the filter uses */
static int16_t nl_x[2 * NL_MAX];	/* the loudspeaker, newest first, twice over */
static int nl_len, nl_pos;
static int64_t nl_norm;

static int nlms_init(int tail_ms)
{
	nl_len = tail_ms * SRATE / 1000;
	memset(nl_w, 0, sizeof(nl_w));
	memset(nl_w16, 0, sizeof(nl_w16));
	memset(nl_x, 0, sizeof(nl_x));
	nl_pos = 0;
	nl_norm = 0;
	return nl_len > NL_MAX;
}
static void nlms_run(int16_t *spk, int16_t *mic)
{
	int i, k, n = nl_len;

	for (i = 0; i < FRAME; i++) {
		const int16_t *x;
		int32_t acc = 0, e, s;

		nl_pos = nl_pos ? nl_pos - 1 : n - 1;
		nl_norm += (int32_t)spk[i] * spk[i] - (int32_t)nl_x[nl_pos] * nl_x[nl_pos];
		nl_x[nl_pos] = nl_x[nl_pos + n] = spk[i];
		x = nl_x + nl_pos;
		for (k = 0; k < n; k++)
			acc += ((int32_t)nl_w16[k] * x[k]) >> 6;
		e = mic[i] - (acc >> 8);
		mic[i] = sat16(e);
		/* step 0.25 */
		s = (int32_t)(((int64_t)sat16(e) << 28) / (nl_norm + ((int64_t)n << 12)));
		if (s > 32767) s = 32767;
		if (s < -32767) s = -32767;
		if (s)
			for (k = 0; k < n; k++) {
				nl_w[k] += (s * x[k]) >> 8;
				nl_w16[k] = (int16_t)(nl_w[k] >> 8);
			}
	}
}

const struct cand cands[] = {
	{ "none", "no echo control", 0, none_init, none_run },
	{ "switch", "today's voice switching", 0, switch_init, switch_run },
	{ "speex", "speexdsp canceller, 20 ms blocks", 1, speex_init, speex_run },
	{ "speex+pp", "speexdsp canceller and preprocessor, 20 ms blocks", 1, speexpp_init, speex_run },
	{ "speex10", "speexdsp canceller, 10 ms blocks", 1, speex10_init, speex_run },
	{ "speex10+pp", "speexdsp canceller and preprocessor, 10 ms blocks", 1, speex10pp_init, speex_run },
	{ "aecm", "WebRTC AECM, speakerphone setting (3)", 0, aecm3_init, aecm_run },
	{ "aecm4", "WebRTC AECM, strongest setting (4)", 0, aecm4_init, aecm_run },
	{ "nlms", "plain NLMS, no double-talk control", 1, nlms_init, nlms_run },
	{ "echo-aec", "audiod's \"echo aec\": speexdsp canceller (120 ms) and a shallow switch", 0, t48aec_init, t48aec_run },
};
const int ncands = sizeof(cands) / sizeof(cands[0]);

const struct cand *cand_find(const char *name)
{
	int i;
	for (i = 0; i < ncands; i++)
		if (!strcmp(cands[i].name, name))
			return &cands[i];
	return 0;
}
