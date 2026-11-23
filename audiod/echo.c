// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: voice-switched echo control. See echo.h. */
#include <string.h>
#include "dsp.h"
#include "echo.h"

#define RX_MIC_GAIN   519	/* -36 dB: microphone while the far end talks */
#define TX_SPK_GAIN   5827	/* -15 dB: loudspeaker while the owner talks */
#define FAR_FLOOR     50	/* loudspeaker level (mean |x|) that counts as sound */
#define MIC_FLOOR     40	/* the owner is never quieter than this */
#define TX_HOLD       12	/* frames: 240 ms, bridges gaps between words */
#define SLEW_DOWN     205	/* per sample: full swing in 10 ms */
#define SLEW_UP       51	/* per sample: full swing in 40 ms */

void echo_init(struct echo *e, enum echo_mode mode, int coupling_q8)
{
	memset(e, 0, sizeof(*e));
	e->mode = mode;
	e->coupling_q8 = coupling_q8 > 0 ? coupling_q8 : 256;
	e->mic_g = e->spk_g = Q15_ONE;
	e->mic_noise_q4 = MIC_FLOOR << 4;
	e->rng = 0x2545f491;
}

static inline int32_t slew(int32_t g, int32_t target)
{
	if (g > target)
		return g - SLEW_DOWN < target ? target : g - SLEW_DOWN;
	if (g < target)
		return g + SLEW_UP > target ? target : g + SLEW_UP;
	return g;
}

void echo_process(struct echo *e, int16_t *spk, int16_t *mic, int n)
{
	int lf, lm, far_env, echo_est, noise, i;
	int32_t mic_t, spk_t, mg, sg, cn_amp;
	uint32_t r;

	if (e->mode != ECHO_SWITCH)
		return;

	lf = dsp_level(spk, n);
	lm = dsp_level(mic, n);

	/* What the microphone may be hearing of the loudspeaker: the loudest
	 * recent frame as it was actually played, or the fading tail of older
	 * ones. This frame is not in it yet: it has not left the loudspeaker. */
	far_env = e->tail;
	for (i = 0; i < ECHO_HIST; i++)
		if (e->hist[i] > far_env)
			far_env = e->hist[i];
	echo_est = (int)(((int64_t)far_env * e->coupling_q8) >> 8);

	/* Background noise: follows the level down at once, up slowly, and
	 * only while the loudspeaker is quiet */
	if (far_env < FAR_FLOOR && lf < FAR_FLOOR) {
		int l_q4 = lm << 4;
		if (l_q4 < e->mic_noise_q4)
			e->mic_noise_q4 = l_q4;
		else
			e->mic_noise_q4 += (e->mic_noise_q4 >> 6) + 1;
		if (e->mic_noise_q4 > 2000 << 4)
			e->mic_noise_q4 = 2000 << 4;
	}
	noise = e->mic_noise_q4 >> 4;

	/* The owner is talking if the microphone is well above the background
	 * and at least twice what the echo could account for */
	if (lm > 3 * noise + MIC_FLOOR && lm > 2 * echo_est)
		e->tx_hold = TX_HOLD;

	if (e->tx_hold > 0) {
		e->tx_hold--;
		e->state = ECHO_TX;
		mic_t = Q15_ONE;
		spk_t = lf > FAR_FLOOR ? TX_SPK_GAIN : Q15_ONE;
	} else if (lf > FAR_FLOOR || far_env > FAR_FLOOR) {
		e->state = ECHO_RX;
		mic_t = RX_MIC_GAIN;
		spk_t = Q15_ONE;
	} else {
		e->state = ECHO_IDLE;
		mic_t = spk_t = Q15_ONE;
	}

	/* Apply, moving the gains a little each sample so nothing clicks.
	 * What is taken from the microphone is made up with noise at the
	 * background level, so the far end does not hear the line go dead. */
	mg = e->mic_g;
	sg = e->spk_g;
	r = e->rng;
	cn_amp = 2 * noise;	/* uniform noise with this peak has the same mean |x| */
	if (cn_amp > 600)
		cn_amp = 600;
	for (i = 0; i < n; i++) {
		int32_t m, cn;

		mg = slew(mg, mic_t);
		sg = slew(sg, spk_t);
		r ^= r << 13; r ^= r >> 17; r ^= r << 5;
		cn = ((int32_t)(int16_t)r * cn_amp) >> 15;
		m = ((int32_t)mic[i] * mg + cn * (Q15_ONE - mg)) >> 15;
		mic[i] = sat16(m);
		spk[i] = (int16_t)(((int32_t)spk[i] * sg) >> 15);
	}
	e->mic_g = mg;
	e->spk_g = sg;
	e->rng = r;

	/* Remember what was played */
	e->tail -= e->tail >> 2;
	if (e->hist[e->hist_pos] > e->tail)
		e->tail = e->hist[e->hist_pos];
	e->hist[e->hist_pos] = (int)(((int64_t)lf * sg) >> 15);
	e->hist_pos = (e->hist_pos + 1) % ECHO_HIST;
}
