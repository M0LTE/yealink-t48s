// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod test: an app's sound. The ring (what is taken each tick, the
 * start, running dry, an app that scribbles on it, off the screen), and the
 * rules in the engine: where it plays, how loud, and that calls win. */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "t.h"
#include "../appsnd.h"
#include "../engine.h"

#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SEAL 1
#define F_SEAL_SHRINK 2
#define F_SEAL_GROW 4
#endif

/* the ring as the shell makes it; sealed or not, of any size */
static int ring_fd(int bytes, int sealed)
{
	int fd = memfd_create("t48-sound", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	if (fd < 0 || ftruncate(fd, bytes) < 0)
		exit(77);
	if (sealed && fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0)
		exit(77);
	return fd;
}

static struct t48_sound_ring *ring;	/* the app's own mapping */
static int16_t *data;
static int16_t next_v;

/* the app writes n samples: 1, 2, 3 ... so that any lost or repeated shows */
static void feed(int n)
{
	while (n--) {
		data[ring->wr & (T48AC_APP_FRAMES - 1)] = ++next_v;
		ring->wr++;
	}
}

static int peak(const int16_t *x, int n)
{
	int i, m = 0;
	for (i = 0; i < n; i++)
		if (abs(x[i]) > m)
			m = abs(x[i]);
	return m;
}

int main(void)
{
	struct appsnd a;
	int16_t out[FRAME], want = 0;
	int fd, i, k, ok;

	memset(&a, 0, sizeof(a));

	/* only a sealed memfd of the full size is taken */
	CHECK(appsnd_open(&a, ring_fd(T48AC_APP_BYTES, 0)) < 0 && !a.ring);
	CHECK(appsnd_open(&a, ring_fd(T48AC_APP_BYTES - 4096, 1)) < 0 && !a.ring);
	CHECK(appsnd_open(&a, open("/dev/null", O_RDWR)) < 0 && !a.ring);
	CHECK(!appsnd_take(&a, out, 0, 0) && !appsnd_waiting(&a));

	fd = ring_fd(T48AC_APP_BYTES, 1);
	ring = mmap(0, T48AC_APP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	data = (int16_t *)((char *)ring + T48AC_APP_DATA);
	CHECK(appsnd_open(&a, dup(fd)) == 0 && a.ring);
	CHECK(ring->state == T48_SOUND_OFF);

	/* off the screen: nothing is taken */
	feed(1000);
	CHECK(!appsnd_waiting(&a) && !appsnd_take(&a, out, 0, 0));
	/* in front: what was written before is not played */
	appsnd_front(&a, 1);
	CHECK(ring->state == T48_SOUND_PLAYING && ring->rd == ring->wr);
	CHECK(!appsnd_take(&a, out, 0, 400) && ring->delay == 400);
	want = next_v;

	/* 1000 samples: three whole frames, then 40 and silence, then nothing */
	feed(1000);
	CHECK(appsnd_waiting(&a) == 1000);
	for (k = 0; k < 3; k++) {
		CHECK(appsnd_take(&a, out, 0, 400) == 1);
		for (i = 0, ok = 1; i < FRAME; i++)
			ok &= out[i] == ++want;
		CHECK(ok);
		CHECK(ring->rd == ring->wr - 1000 + (uint32_t)(k + 1) * FRAME);
	}
	CHECK(appsnd_take(&a, out, 0, 400) == 1);
	for (i = 0, ok = 1; i < FRAME; i++)
		ok &= out[i] == (i < 40 ? ++want : 0);
	CHECK(ok && a.dry == 1 && ring->dry == 1);
	CHECK(!appsnd_take(&a, out, 0, 400) && !appsnd_waiting(&a));

	/* less than a frame waits two ticks for the rest, then is played */
	feed(100);
	CHECK(!appsnd_take(&a, out, 0, 400));
	CHECK(!appsnd_take(&a, out, 0, 400));
	CHECK(appsnd_take(&a, out, 0, 400) == 1);
	for (i = 0, ok = 1; i < FRAME; i++)
		ok &= out[i] == (i < 100 ? ++want : 0);
	CHECK(ok);
	/* and if the rest comes, it starts at once on a whole frame */
	feed(100);
	CHECK(!appsnd_take(&a, out, 0, 400));
	feed(300);
	CHECK(appsnd_take(&a, out, 0, 400) == 1 && out[0] == want + 1 && out[FRAME - 1] == want + FRAME);
	CHECK(appsnd_take(&a, out, 0, 400) == 1 && out[79] == want + 400 && out[80] == 0);
	want += 400;

	/* a long run, round the ring many times: every sample once, in order */
	for (k = 0, ok = 1; k < 200; k++) {
		feed(FRAME);
		ok &= appsnd_take(&a, out, 0, 400) == 1;
		for (i = 0; i < FRAME; i++)
			ok &= out[i] == ++want;
	}
	CHECK(ok && a.resets == 0);

	/* held (a call): taken all the same, and the app is told */
	feed(FRAME);
	CHECK(appsnd_take(&a, out, 1, 400) == 1 && ring->state == T48_SOUND_HELD && !appsnd_waiting(&a));
	want += FRAME;
	CHECK(!appsnd_take(&a, out, 0, 400) && ring->state == T48_SOUND_PLAYING);

	/* an app that scribbles on the ring cannot hurt: its copy of rd is not
	 * ours, and a write count that cannot be right empties the ring */
	feed(FRAME);
	ring->rd = 123456;
	CHECK(appsnd_take(&a, out, 0, 400) == 1 && out[0] == want + 1 && ring->rd == ring->wr);
	want += FRAME;
	ring->wr += 100000;
	CHECK(!appsnd_take(&a, out, 0, 400) && a.resets == 1 && ring->rd == ring->wr);
	ring->wr -= 5;	/* backwards */
	CHECK(!appsnd_take(&a, out, 0, 400) && a.resets == 2);
	ring->magic = ring->frames = ring->data = 0xffffffff;	/* never read by the service */
	feed(FRAME);
	CHECK(appsnd_take(&a, out, 0, 400) == 1 && out[FRAME - 1] == next_v);

	/* the app's wake-up flag is cleared when it is served */
	ring->waiting = 1;
	CHECK(!appsnd_take(&a, out, 0, 400) && ring->waiting == 0);

	/* off the screen: told, and what was waiting is gone when it is back */
	feed(2000);
	appsnd_front(&a, 0);
	CHECK(ring->state == T48_SOUND_OFF && !appsnd_take(&a, out, 0, 400));
	appsnd_front(&a, 1);
	CHECK(!appsnd_waiting(&a) && !appsnd_take(&a, out, 0, 400));
	appsnd_close(&a);
	CHECK(ring->state == T48_SOUND_OFF && !a.ring);

	/* ---- the engine: where an app's sound goes ---- */
	{
		struct engine e;
		static int16_t snd[FRAME], mic[FRAME], far[FRAME], mo[FRAME], so[FRAME];
		int p;

		for (i = 0; i < FRAME; i++)
			snd[i] = (i & 16) ? 32767 : -32768;	/* as loud as an app can be */
		sine_fill(far, FRAME, 1000, 8000, 0);
		engine_init(&e);
#define TICK(with_app, call_frame) do { \
		if (with_app) engine_app_frame(&e, snd); \
		engine_tick(&e, mic, call_frame, mo, so); } while (0)

		/* on hook: the loudspeaker, at the app volume; 15 is the limit and no more */
		CHECK(e.vol[T48AC_VOL_APP] == 10);
		engine_set_volume(&e, T48AC_VOL_APP, 15);
		TICK(1, 0);
		TICK(1, 0);
		CHECK(e.app_heard && peak(so, FRAME) <= SPK_LIMIT && peak(so, FRAME) >= SPK_LIMIT - 2);
		for (k = 14, p = SPK_LIMIT; k >= 1; k--) {	/* 2 dB a step */
			engine_set_volume(&e, T48AC_VOL_APP, k);
			TICK(1, 0);
			TICK(1, 0);
			CHECK_RANGE((double)peak(so, FRAME) / p, 0.77, 0.82);
			p = peak(so, FRAME);
		}
		engine_set_volume(&e, T48AC_VOL_APP, 0);
		TICK(1, 0);
		TICK(1, 0);
		CHECK(peak(so, FRAME) == 0);
		/* the ringer's volume is not the app's */
		engine_set_volume(&e, T48AC_VOL_APP, 15);
		engine_set_volume(&e, T48AC_VOL_RINGER, 0);
		TICK(1, 0);
		TICK(1, 0);
		CHECK(peak(so, FRAME) >= SPK_LIMIT - 2);
		/* a frame is for one tick only: an app that stops is silence */
		TICK(0, 0);
		CHECK(peak(so, FRAME) == 0 && !e.app_heard);
		/* with a key sound on top, still under the limit */
		engine_key(&e, '5');
		for (k = 0, p = 0; k < 10; k++) {
			TICK(1, 0);
			if (peak(so, FRAME) > p)
				p = peak(so, FRAME);
		}
		CHECK(p <= SPK_LIMIT);

		/* calls win. Ringing: faded out within one frame, silent after, back after */
		TICK(1, 0);
		engine_set_ring(&e, 1);
		CHECK(engine_app_held(&e));
		TICK(1, 0);
		CHECK(e.app_heard);	/* the fade */
		for (k = 0; k < 25; k++) {
			TICK(1, 0);
			CHECK(!e.app_heard);
		}
		CHECK(tone_amp(so, FRAME, 500) < 30);	/* the ringer, and none of the app's 500 Hz */
		engine_set_ring(&e, 0);
		for (k = 0; k < 10; k++)
			TICK(1, 0);	/* the ringer fades, then the app is back */
		CHECK(!engine_app_held(&e) && e.app_heard && peak(so, FRAME) >= SPK_LIMIT - 2);
		/* a tone */
		engine_set_tone(&e, T48AC_TONE_DIAL);
		TICK(1, 0);
		TICK(1, 0);
		CHECK(engine_app_held(&e) && !e.app_heard);
		engine_set_tone(&e, T48AC_TONE_OFF);
		for (k = 0; k < 10; k++)
			TICK(1, 0);
		CHECK(e.app_heard);
		/* a call on the handset: only the call is heard */
		engine_set_volume(&e, T48AC_VOL_HANDSET, 15);
		engine_set_route(&e, T48AC_ROUTE_HANDSET);
		engine_set_call(&e, 1);
		TICK(1, far);
		TICK(1, far);
		CHECK(engine_app_held(&e) && !e.app_heard);
		CHECK(peak(so, FRAME) <= 8000);
		/* the handset lifted with no call and no tone: the app's sound is in
		 * the earpiece, at the app volume on the earpiece's scale */
		engine_set_call(&e, 0);
		engine_set_volume(&e, T48AC_VOL_APP, 13);
		TICK(1, 0);
		TICK(1, 0);
		CHECK(e.app_heard);
		CHECK_RANGE(peak(so, FRAME), 32767 * 0.49, 32767 * 0.51);
		/* the speaker route has the amplifier: the loudspeaker's scale again */
		engine_set_route(&e, T48AC_ROUTE_SPEAKER);
		engine_set_volume(&e, T48AC_VOL_APP, 15);
		for (k = 0, p = 0; k < 25; k++) {	/* the limiter catches the change and lets go */
			TICK(1, 0);
			if (peak(so, FRAME) > p)
				p = peak(so, FRAME);
		}
		CHECK(p <= SPK_LIMIT && peak(so, FRAME) >= SPK_LIMIT - 2);
	}
	T_DONE("test_appsnd");
}
