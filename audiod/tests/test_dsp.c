// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod test: gain, mixing, level, DC removal. */
#include <string.h>
#include "t.h"
#include "../dsp.h"

int main(void)
{
	static int16_t a[SRATE], b[SRATE], out[SRATE];
	struct dcblock dc = { 0 };
	int i;

	/* volume table: silent at 0, full at 15, 3 dB a step */
	CHECK(dsp_vol_q15(0) == 0);
	CHECK(dsp_vol_q15(15) == Q15_ONE);
	CHECK(dsp_vol_q15(-3) == 0 && dsp_vol_q15(99) == Q15_ONE);
	for (i = 2; i <= 15; i++)
		CHECK_RANGE(20 * log10((double)dsp_vol_q15(i) / dsp_vol_q15(i - 1)), 2.95, 3.05);

	/* gain */
	sine_fill(a, SRATE, 1000, 20000, 0);
	memcpy(out, a, sizeof(a));
	dsp_gain(out, SRATE, dsp_vol_q15(13));	/* -6 dB */
	CHECK_RANGE(tone_amp(out, SRATE, 1000), 20000 * 0.495, 20000 * 0.505);
	memcpy(out, a, sizeof(a));
	dsp_gain(out, SRATE, Q15_ONE);
	CHECK(!memcmp(out, a, sizeof(a)));
	dsp_gain(out, SRATE, 0);
	CHECK(rms(out, SRATE) == 0);

	/* mixing adds, and clips instead of wrapping */
	sine_fill(b, SRATE, 440, 8000, 0);
	dsp_mix(out, a, b, Q15_ONE, SRATE);
	CHECK_RANGE(tone_amp(out, SRATE, 1000), 19900, 20100);
	CHECK_RANGE(tone_amp(out, SRATE, 440), 7950, 8050);
	for (i = 0; i < SRATE; i++)
		a[i] = b[i] = (i & 64) ? 30000 : -30000;
	dsp_mix(out, a, b, Q15_ONE, SRATE);
	for (i = 0; i < SRATE; i++)
		if (out[i] != ((i & 64) ? 32767 : -32768))
			break;
	CHECK(i == SRATE);
	CHECK(sat16(40000) == 32767 && sat16(-40000) == -32768 && sat16(-5) == -5);

	/* level is the mean of |x| */
	for (i = 0; i < FRAME; i++)
		a[i] = (i & 1) ? 1000 : -1000;
	CHECK(dsp_level(a, FRAME) == 1000);
	a[0] = -32768;
	CHECK(dsp_level(a, FRAME) > 1000);

	/* DC removal: the 9000 count offset the preamp gives at 42 dB goes,
	 * a 300 Hz voice tone stays */
	sine_fill(a, SRATE, 300, 5000, 0);
	for (i = 0; i < SRATE; i++)
		a[i] += 9000;
	for (i = 0; i < SRATE; i += FRAME)
		dsp_dcblock(&dc, a + i, FRAME);
	{
		double mean = 0;
		for (i = SRATE / 2; i < SRATE; i++)
			mean += a[i];
		CHECK_RANGE(mean / (SRATE / 2), -20, 20);
	}
	CHECK_RANGE(tone_amp(a + SRATE / 2, SRATE / 2, 300), 4900, 5050);

	/* the ringer's own volume table: silent at 0, 2 dB a step, and at 15
	 * the ringer (peak 16000) sits just on the loudspeaker's limit */
	CHECK(dsp_alert_q15(0) == 0 && dsp_alert_q15(-1) == 0 && dsp_alert_q15(99) == dsp_alert_q15(15));
	CHECK((16000 * dsp_alert_q15(15)) >> 15 <= SPK_LIMIT && (16000 * dsp_alert_q15(15)) >> 15 > SPK_LIMIT - 10);
	for (i = 2; i <= 15; i++)
		CHECK_RANGE(20 * log10((double)dsp_alert_q15(i) / dsp_alert_q15(i - 1)), 1.9, 2.1);

	/* the limiter. Under the ceiling: not a bit is changed */
	{
		struct limiter lim = { 0 };
		uint32_t seed = 7;
		int peak, j;

		sine_fill(a, SRATE, 1000, 1790, 0);
		memcpy(out, a, sizeof(a));
		for (i = 0; i < SRATE; i += FRAME)
			dsp_limit(&lim, out + i, FRAME, 1800);
		CHECK(!memcmp(out, a, sizeof(a)));

		/* a tone far over it: turned down to the ceiling and still a
		 * sine, not clipped (third harmonic under 1%) */
		sine_fill(a, SRATE, 1000, 30000, 0);
		memcpy(out, a, sizeof(a));
		for (i = 0; i < SRATE; i += FRAME)
			dsp_limit(&lim, out + i, FRAME, 1800);
		for (i = 0, peak = 0; i < SRATE; i++)
			if (abs(out[i]) > peak)
				peak = abs(out[i]);
		CHECK(peak <= 1800 && peak > 1750);
		CHECK_RANGE(tone_amp(out + SRATE / 2, SRATE / 2, 1000), 1750, 1830);
		CHECK(tone_amp(out + SRATE / 2, SRATE / 2, 3000) < 18);

		/* the worst there is, from silence, at any frame size: full-scale
		 * square waves and noise never get a sample through above it */
		for (j = 0; j < 3; j++) {
			static const int chunk[3] = { 1, 7, FRAME };
			memset(&lim, 0, sizeof(lim));
			for (i = 0; i < SRATE; i++)
				a[i] = i < 4000 ? ((i & 8) ? 32767 : -32768) : 0;
			noise_fill(a + 8000, 8000, 32767, &seed);
			for (i = 0; i < SRATE; i += chunk[j])
				dsp_limit(&lim, a + i, i + chunk[j] <= SRATE ? chunk[j] : SRATE - i, 1800);
			for (i = 0, peak = 0; i < SRATE; i++)
				if (abs(a[i]) > peak)
					peak = abs(a[i]);
			CHECK(peak <= 1800 && peak > 1700);
		}

		/* after something loud the gain comes back: a quiet tone is
		 * itself again a quarter of a second later */
		sine_fill(a, 4000, 1000, 30000, 0);
		sine_fill(a + 4000, 12000, 1000, 1000, 4000);
		for (i = 0; i < SRATE; i += FRAME)
			dsp_limit(&lim, a + i, FRAME, 1800);
		CHECK(tone_amp(a + 4160, 320, 1000) < 200);	/* still held down just after */
		CHECK_RANGE(tone_amp(a + 8000, 8000, 1000), 990, 1001);
	}

	T_DONE("dsp");
}
