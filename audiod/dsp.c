// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: integer sample helpers. See dsp.h. */
#include "dsp.h"

int dsp_vol_q15(int level)
{
	/* 32767 * 10^(-3 * (15 - level) / 20) */
	static const int16_t tab[16] = {
		0, 260, 368, 519, 734, 1036, 1464, 2067,
		2920, 4125, 5827, 8231, 11626, 16422, 23197, 32767
	};
	return tab[level < 0 ? 0 : level > 15 ? 15 : level];
}

void dsp_gain(int16_t *x, int n, int g)
{
	int i;
	if (g >= Q15_ONE)
		return;
	for (i = 0; i < n; i++)
		x[i] = (int16_t)(((int32_t)x[i] * g) >> 15);
}

void dsp_mix(int16_t *out, const int16_t *a, const int16_t *b, int g, int n)
{
	int i;
	for (i = 0; i < n; i++)
		out[i] = sat16(a[i] + (((int32_t)b[i] * g) >> 15));
}

int dsp_level(const int16_t *x, int n)
{
	uint32_t sum = 0;
	int i;
	for (i = 0; i < n; i++)
		sum += (uint32_t)(x[i] < 0 ? -x[i] : x[i]);
	return (int)(sum / (uint32_t)n);
}

void dsp_dcblock(struct dcblock *st, int16_t *x, int n)
{
	int32_t m = st->mean_q8;
	int i;
	for (i = 0; i < n; i++) {
		m += (((int32_t)x[i] << 8) - m) >> 7;
		x[i] = sat16(x[i] - (m >> 8));
	}
	st->mean_q8 = m;
}
