// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-audiod: what one 20 ms frame costs. Run it here for a figure scaled to
 * the phone, or build it for ARM and run it on the phone for the real one:
 *
 *   make bench
 *   make CC=$HOME/src-build/t48s/bin/armcc O=build-arm build-arm/bench
 *
 * The scaling uses the "mix" loop of native/cpubench.c, which the phone runs
 * at 31.2 million passes a second under our kernel (issue #9). The Makefile
 * builds this without auto-vectorising, since the phone has no NEON.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "t.h"
#include "../engine.h"

#define PHONE_MIX 31.2	/* million passes a second, measured on the phone */

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* native/cpubench.c's integer work in the L1 cache */
static unsigned tab[1024];
__attribute__((noinline)) static unsigned mix(unsigned n)
{
	unsigned x = 2463534242u, s = 0;
	while (n--) {
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		s += tab[x & 1023] * (x | 1);
		tab[(x >> 10) & 1023] = s;
	}
	return s;
}

static int16_t mic[64][FRAME], spk[64][FRAME], mo[FRAME], so[FRAME];
static volatile int sink;
static double scale;

static void report(const char *what, double sec, int frames)
{
	double us = sec / frames * 1e6;
	printf("%-52s %7.2f us here   %7.1f us on the phone (%.2f%% of a frame)\n",
	       what, us, us * scale, us * scale / 20000 * 100);
}

int main(void)
{
	struct engine e;
	struct tone_gen g;
	struct echo ec;
	struct dcblock dc = { 0 };
	uint32_t seed = 3;
	double t, here;
	int i, n = 200000;
#if defined(__arm__)
	n = 5000;
#endif

	for (i = 0; i < 1024; i++)
		tab[i] = (unsigned)i * 2654435761u;
	t = now();
	sink = (int)mix(50000000u / (n == 5000 ? 10 : 1));
	here = (50.0 / (n == 5000 ? 10 : 1)) / (now() - t);
	scale = here / PHONE_MIX;
#if defined(__arm__)
	scale = 1;	/* on the phone itself: no scaling */
#endif
	printf("this machine: %.1f million mix passes a second; the phone: %.1f; scale x%.1f\n",
	       here, PHONE_MIX, scale);

	for (i = 0; i < 64; i++) {
		noise_fill(mic[i], FRAME, 3000, &seed);
		noise_fill(spk[i], FRAME, 8000, &seed);
	}

	engine_init(&e);
	engine_set_route(&e, T48AC_ROUTE_HANDSET);
	t = now();
	for (i = 0; i < n; i++)
		engine_tick(&e, mic[i & 63], spk[i & 63], mo, so);
	sink = mo[5] + so[7];
	report("handset call (volume, DC removal)", now() - t, n);

	engine_set_route(&e, T48AC_ROUTE_SPEAKER);
	t = now();
	for (i = 0; i < n; i++)
		engine_tick(&e, mic[i & 63], spk[i & 63], mo, so);
	sink = mo[5] + so[7];
	report("speakerphone call (adds voice switching)", now() - t, n);

	/* "echo aec": far fewer frames, it is some hundred times the work */
	routes[T48AC_ROUTE_SPEAKER].echo = ECHO_AEC;
	engine_set_route(&e, T48AC_ROUTE_NONE);
	t = now();
	engine_set_route(&e, T48AC_ROUTE_SPEAKER);
	printf("%-52s %7.2f ms here   %7.1f ms on the phone, once\n", "starting the echo canceller (a route change)",
	       (now() - t) * 1e3, (now() - t) * 1e3 * scale);
	t = now();
	for (i = 0; i < n / 20; i++)
		engine_tick(&e, mic[i & 63], spk[i & 63], mo, so);
	sink = mo[5] + so[7];
	report("speakerphone call with \"echo aec\"", now() - t, n / 20);
	routes[T48AC_ROUTE_SPEAKER].echo = ECHO_SWITCH;
	engine_set_route(&e, T48AC_ROUTE_NONE);
	engine_set_route(&e, T48AC_ROUTE_SPEAKER);

	engine_set_tone(&e, T48AC_TONE_DIAL);
	t = now();
	for (i = 0; i < n; i++) {
		if (!(i & 3))
			engine_key(&e, '5');
		engine_tick(&e, mic[i & 63], spk[i & 63], mo, so);
	}
	sink = mo[5] + so[7];
	report("worst case: speakerphone + tone + key sound", now() - t, n);

	engine_set_route(&e, T48AC_ROUTE_NONE);
	engine_set_tone(&e, T48AC_TONE_OFF);
	engine_set_ring(&e, 1);
	t = now();
	for (i = 0; i < n; i++)
		engine_tick(&e, mic[i & 63], 0, mo, so);
	sink = mo[5] + so[7];
	report("ringing on hook", now() - t, n);

	printf("the parts:\n");
	tone_start(&g, &tone_dial);
	t = now();
	for (i = 0; i < n; i++)
		tone_render(&g, so, FRAME);
	report("  two-tone generator", now() - t, n);
	echo_init(&ec, ECHO_SWITCH, 256);
	t = now();
	for (i = 0; i < n; i++) {
		memcpy(mo, mic[i & 63], sizeof(mo));
		memcpy(so, spk[i & 63], sizeof(so));
		echo_process(&ec, so, mo, FRAME);
	}
	report("  voice switching", now() - t, n);
	t = now();
	for (i = 0; i < n; i++) {
		memcpy(mo, mic[i & 63], sizeof(mo));
		dsp_dcblock(&dc, mo, FRAME);
	}
	report("  DC removal", now() - t, n);
	t = now();
	for (i = 0; i < n; i++)
		dsp_mix(so, spk[i & 63], mic[i & 63], 20000, FRAME);
	report("  mixing one source in", now() - t, n);
	return 0;
}
