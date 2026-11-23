/* SPDX-License-Identifier: AGPL-3.0-or-later
 * t48-audiod tests: the few helpers they share. Tests may use floating
 * point; the code under test may not. */
#ifndef AUDIOD_T_H
#define AUDIOD_T_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int t_fails __attribute__((unused)), t_checks __attribute__((unused));

#define CHECK(cond) do { \
	t_checks++; \
	if (!(cond)) { t_fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_RANGE(v, lo, hi) do { \
	double v_ = (v); \
	t_checks++; \
	if (!(v_ >= (lo) && v_ <= (hi))) { t_fails++; \
		printf("  FAIL %s:%d: %s = %g, want %g to %g\n", __FILE__, __LINE__, #v, v_, (double)(lo), (double)(hi)); } \
} while (0)

#define T_DONE(name) do { \
	printf("%s %s: %d checks, %d failed\n", t_fails ? "FAIL" : "PASS", name, t_checks, t_fails); \
	return t_fails ? 1 : 0; \
} while (0)

/* Amplitude of the hz component of x (a full-scale sine gives 32767) */
static inline double tone_amp(const int16_t *x, int n, double hz)
{
	double re = 0, im = 0, w = 2 * M_PI * hz / 16000.0;
	int i;
	for (i = 0; i < n; i++) {
		re += x[i] * cos(w * i);
		im += x[i] * sin(w * i);
	}
	return 2 * sqrt(re * re + im * im) / n;
}

static inline double rms(const int16_t *x, int n)
{
	double s = 0;
	int i;
	for (i = 0; i < n; i++)
		s += (double)x[i] * x[i];
	return sqrt(s / n);
}

static inline int max_step(const int16_t *x, int n)
{
	int i, m = 0;
	for (i = 1; i < n; i++)
		if (abs(x[i] - x[i - 1]) > m)
			m = abs(x[i] - x[i - 1]);
	return m;
}

static inline void sine_fill(int16_t *x, int n, double hz, double amp, int start)
{
	int i;
	for (i = 0; i < n; i++)
		x[i] = (int16_t)lrint(amp * sin(2 * M_PI * hz * (start + i) / 16000.0));
}

/* Something like speech for level tests: noise, loud and steady */
static inline void noise_fill(int16_t *x, int n, int peak, uint32_t *seed)
{
	int i;
	for (i = 0; i < n; i++) {
		*seed = *seed * 1664525u + 1013904223u;
		x[i] = (int16_t)(((int32_t)(int16_t)(*seed >> 16) * peak) >> 15);
	}
}
#endif
