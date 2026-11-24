// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod test: the per-frame rules. What reaches the call and the
 * loudspeaker for each route, mute, volume, tones and the ringer. */
#include <string.h>
#include "t.h"
#include "../engine.h"

#define N (SRATE)	/* one second */
static int16_t mic[N], spk[N], mic_out[N], spk_out[N];

/* Run one second: microphone hears 300 Hz, the call sends 700 Hz */
static void run(struct engine *e, int with_call)
{
	int i;
	for (i = 0; i < N; i += FRAME)
		engine_tick(e, mic + i, with_call ? spk + i : 0, mic_out + i, spk_out + i);
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
	struct engine e;
	int i, k;

	sine_fill(mic, N, 300, 6000, 0);
	sine_fill(spk, N, 700, 8000, 0);
	engine_init(&e);
	for (i = 1; i < T48AC_VOL_COUNT; i++)
		engine_set_volume(&e, i, 15);

	/* on hook: the microphone never reaches the call and the call is not played */
	run(&e, 1);
	CHECK(rms(mic_out, N) == 0 && rms(spk_out, N) == 0);
	CHECK(engine_hw_row(&e) == ROUTE_ALERT && !engine_sounding(&e));

	/* handset: both directions pass */
	engine_set_route(&e, T48AC_ROUTE_HANDSET);
	run(&e, 1);
	CHECK_RANGE(tone_amp(mic_out + N / 2, N / 2, 300), 5900, 6050);
	CHECK(rms(mic_out, 24 * 320) == 0);	/* held back while the microphone settles */
	CHECK_RANGE(tone_amp(spk_out, N, 700), 7950, 8050);
	CHECK(tone_amp(spk_out, N, 300) < 20);	/* no software sidetone */
	CHECK(engine_hw_row(&e) == T48AC_ROUTE_HANDSET);

	/* volume: each step is 3 dB on the loudspeaker side, none on the microphone */
	engine_set_volume(&e, T48AC_VOL_HANDSET, 13);
	run(&e, 1);
	CHECK_RANGE(tone_amp(spk_out, N, 700), 8000 * 0.495, 8000 * 0.505);
	CHECK_RANGE(tone_amp(mic_out, N, 300), 5900, 6050);
	/* another route's volume does not touch this one */
	engine_set_volume(&e, T48AC_VOL_SPEAKER, 3);
	engine_set_volume(&e, T48AC_VOL_RINGER, 3);
	run(&e, 1);
	CHECK_RANGE(tone_amp(spk_out, N, 700), 8000 * 0.495, 8000 * 0.505);
	engine_set_volume(&e, T48AC_VOL_HANDSET, 0);
	run(&e, 1);
	CHECK(rms(spk_out, N) == 0);
	engine_set_volume(&e, T48AC_VOL_HANDSET, 99);	/* clamped */
	CHECK(e.vol[T48AC_VOL_HANDSET] == 15);

	/* mute: the call gets silence, the owner still hears the call */
	engine_set_mute(&e, 1);
	run(&e, 1);
	CHECK(rms(mic_out, N) == 0);
	CHECK_RANGE(tone_amp(spk_out, N, 700), 7950, 8050);
	/* hanging up clears it, and mute means nothing on hook */
	engine_set_route(&e, T48AC_ROUTE_NONE);
	CHECK(e.mute == 0);
	engine_set_mute(&e, 1);
	CHECK(e.mute == 0);

	/* no frame from baresip this tick: silence, not the last frame again */
	engine_set_route(&e, T48AC_ROUTE_HEADSET);
	run(&e, 0);
	CHECK(rms(spk_out, N) == 0 && rms(mic_out + N / 2, N / 2) > 3000);

	/* a progress tone is mixed on top of the call, at the route's volume */
	engine_set_tone(&e, T48AC_TONE_ERROR);
	CHECK(engine_sounding(&e));
	run(&e, 1);
	CHECK_RANGE(tone_amp(spk_out, N, 400), 7900, 8050);
	CHECK_RANGE(tone_amp(spk_out, N, 700), 7950, 8050);
	engine_set_volume(&e, T48AC_VOL_HEADSET, 13);
	run(&e, 1);
	CHECK_RANGE(tone_amp(spk_out, N, 400), 3900, 4050);
	engine_set_volume(&e, T48AC_VOL_HEADSET, 15);
	engine_set_tone(&e, T48AC_TONE_DIAL);	/* replaces it */
	run(&e, 1);
	CHECK(tone_amp(spk_out, N, 400) < 50 && tone_amp(spk_out, N, 350) > 4900);
	engine_set_tone(&e, T48AC_TONE_OFF);
	run(&e, 1);
	CHECK(!engine_sounding(&e) && tone_amp(spk_out + N / 2, N / 2, 350) < 5);
	engine_set_tone(&e, 77);	/* nonsense is ignored */
	CHECK(e.tone == T48AC_TONE_OFF);

	/* loud call plus loud tone clips, never wraps round */
	sine_fill(spk, N, 700, 32000, 0);
	engine_set_tone(&e, T48AC_TONE_ERROR);
	run(&e, 1);
	for (i = 1, k = 0; i < N; i++)
		if (abs(spk_out[i] - spk_out[i - 1]) > 20000)
			k++;
	CHECK(k == 0);
	engine_set_tone(&e, T48AC_TONE_OFF);
	sine_fill(spk, N, 700, 8000, 0);

	/* keys: a DTMF sound on top, which ends by itself */
	run(&e, 1);
	engine_key(&e, '5');
	CHECK(engine_sounding(&e));
	run(&e, 1);
	CHECK(tone_amp(spk_out + 160, 960, 770) > 4000 && tone_amp(spk_out + 160, 960, 1336) > 4000);
	CHECK(!engine_sounding(&e));

	/* the ringer. On hook: the ring melody, at the ringer volume, for the
	 * loudspeaker (the alert row), and still nothing from the microphone */
	engine_set_route(&e, T48AC_ROUTE_NONE);
	engine_set_volume(&e, T48AC_VOL_RINGER, 15);
	engine_set_ring(&e, 1);
	run(&e, 1);
	CHECK(engine_hw_row(&e) == ROUTE_ALERT && engine_sounding(&e));
	/* at 15 it sits on the loudspeaker's limit, and is not turned down by the limiter */
	CHECK_RANGE(tone_amp(spk_out + 816, 256, 1000), SPK_LIMIT * 0.97, SPK_LIMIT);
	CHECK(peak(spk_out, N) <= SPK_LIMIT && peak(spk_out, N) > SPK_LIMIT - 10);
	CHECK(rms(mic_out, N) == 0);
	CHECK(tone_amp(spk_out, N, 700) < 30);	/* call audio is not played on hook */
	engine_set_volume(&e, T48AC_VOL_RINGER, 9);	/* 12 dB down: the ringer's steps are 2 dB */
	engine_set_ring(&e, 0);
	run(&e, 1);
	engine_set_ring(&e, 1);
	run(&e, 1);
	CHECK_RANGE(tone_amp(spk_out + 816, 256, 1000), SPK_LIMIT * 0.24, SPK_LIMIT * 0.26);
	engine_set_volume(&e, T48AC_VOL_RINGER, 15);

	/* lifting the handset while it rings: the melody stops at once and
	 * can never be heard in the earpiece; ringing is now the quiet beep */
	engine_set_route(&e, T48AC_ROUTE_HANDSET);
	run(&e, 1);
	for (i = 0, k = 0; i < N; i += 160)
		if (tone_amp(spk_out + i, 160, 1000) > 1500 || tone_amp(spk_out + i, 160, 1250) > 1500)
			k++;
	CHECK(k == 0);
	CHECK(tone_amp(spk_out + 160, 1280, 400) > 7000);	/* call waiting beep */
	engine_set_ring(&e, 0);
	run(&e, 1);
	CHECK(!engine_sounding(&e));
	/* and a second call ringing during a call is the beep from the start */
	engine_set_ring(&e, 1);
	run(&e, 1);
	CHECK(tone_amp(spk_out + 160, 1280, 400) > 7000 && tone_amp(spk_out + 32, 320, 1000) < 300);
	/* hanging up with it still ringing: back to the melody on the loudspeaker */
	engine_set_route(&e, T48AC_ROUTE_NONE);
	run(&e, 1);
	CHECK(tone_amp(spk_out + 816, 256, 1000) > SPK_LIMIT * 0.97);
	engine_set_ring(&e, 0);

	/* on-hook dialling: key sounds and tones play at the ringer volume
	 * (8000 at 4 dB under the ringer's top, which is SPK_LIMIT for 16000) */
	run(&e, 1);
	engine_set_volume(&e, T48AC_VOL_RINGER, 13);
	engine_set_tone(&e, T48AC_TONE_ERROR);
	run(&e, 1);
	CHECK_RANGE(tone_amp(spk_out, N, 400), 560, 575);
	engine_set_tone(&e, T48AC_TONE_OFF);
	run(&e, 1);

	/* the loudspeaker's limit (issue #57): whatever is asked of the two
	 * routes that use the amplifier, no sample leaves above SPK_LIMIT.
	 * The loudest call there can be, a tone and a key on top, volume 15: */
	CHECK(routes[ROUTE_ALERT].amp && routes[T48AC_ROUTE_SPEAKER].amp);
	CHECK(!routes[T48AC_ROUTE_HANDSET].amp && !routes[T48AC_ROUTE_HEADSET].amp);
	sine_fill(spk, N, 700, 32767, 0);
	routes[T48AC_ROUTE_SPEAKER].echo = ECHO_NONE;	/* voice switching would turn it down too */
	engine_set_route(&e, T48AC_ROUTE_SPEAKER);
	routes[T48AC_ROUTE_SPEAKER].echo = ECHO_SWITCH;
	engine_set_volume(&e, T48AC_VOL_SPEAKER, 15);
	run(&e, 1);
	CHECK(peak(spk_out, N) <= SPK_LIMIT && peak(spk_out, N) > SPK_LIMIT - 50);
	/* turned down, not clipped: still the call's tone and nothing else */
	CHECK_RANGE(tone_amp(spk_out + N / 2, N / 2, 700), SPK_LIMIT * 0.97, SPK_LIMIT * 1.02);
	CHECK(tone_amp(spk_out + N / 2, N / 2, 2100) < SPK_LIMIT / 100);
	engine_set_tone(&e, T48AC_TONE_ERROR);
	engine_key(&e, '5');
	run(&e, 1);
	CHECK(peak(spk_out, N) <= SPK_LIMIT);
	engine_set_tone(&e, T48AC_TONE_OFF);
	/* a quiet call is left alone */
	sine_fill(spk, N, 700, 1500, 0);
	run(&e, 1);
	run(&e, 1);
	CHECK_RANGE(tone_amp(spk_out, N, 700), 1490, 1501);
	/* on hook, every ringer volume, ringing with a key pressed */
	engine_set_route(&e, T48AC_ROUTE_NONE);
	for (k = 0; k <= 15; k++) {
		engine_set_volume(&e, T48AC_VOL_RINGER, k);
		engine_set_ring(&e, 1);
		engine_key(&e, '#');
		run(&e, 1);
		CHECK(peak(spk_out, N) <= SPK_LIMIT);
		engine_set_ring(&e, 0);
		run(&e, 1);
	}
	/* the earpiece and the headset are not held back */
	sine_fill(spk, N, 700, 8000, 0);
	engine_set_route(&e, T48AC_ROUTE_HANDSET);
	engine_set_volume(&e, T48AC_VOL_HANDSET, 15);
	run(&e, 1);
	CHECK_RANGE(tone_amp(spk_out, N, 700), 7950, 8050);
	engine_set_route(&e, T48AC_ROUTE_NONE);

	/* a routes file cannot turn the loudspeaker up past the gains the
	 * limit was measured with, and a route that switches the amplifier
	 * on is limited whatever it is called */
	{
		struct route saved_spk = routes[T48AC_ROUTE_SPEAKER], saved_hs = routes[T48AC_ROUTE_HANDSET];
		struct route *r = &routes[T48AC_ROUTE_SPEAKER];

		for (i = 0; i < r->nctl; i++) {
			if (!strcmp(r->ctl[i].name, "Codec A Playback Volume"))
				r->ctl[i].value = 42;
			if (!strcmp(r->ctl[i].name, "Codec A Output A Playback Volume"))
				r->ctl[i].value = 7;
		}
		r = &routes[T48AC_ROUTE_HANDSET];
		strcpy(r->ctl[r->nctl].name, "Speaker Amp Switch");
		r->ctl[r->nctl++].value = 1;
		CHECK(routes_fix() == 2);
		r = &routes[T48AC_ROUTE_SPEAKER];
		for (i = 0; i < r->nctl; i++) {
			if (!strcmp(r->ctl[i].name, "Codec A Playback Volume"))
				CHECK(r->ctl[i].value == SPK_MAX_DAC);
			if (!strcmp(r->ctl[i].name, "Codec A Output A Playback Volume"))
				CHECK(r->ctl[i].value == SPK_MAX_OUT);
		}
		CHECK(routes[T48AC_ROUTE_HANDSET].amp);
		sine_fill(spk, N, 700, 32767, 0);
		engine_set_route(&e, T48AC_ROUTE_HANDSET);
		run(&e, 1);
		CHECK(peak(spk_out, N) <= SPK_LIMIT);
		engine_set_route(&e, T48AC_ROUTE_NONE);
		routes[T48AC_ROUTE_SPEAKER] = saved_spk;
		routes[T48AC_ROUTE_HANDSET] = saved_hs;
		CHECK(routes_fix() == 0 && !routes[T48AC_ROUTE_HANDSET].amp);
		sine_fill(spk, N, 700, 8000, 0);
	}

	/* the speakerphone route turns the echo control on, the others off */
	engine_set_route(&e, T48AC_ROUTE_SPEAKER);
	CHECK(e.echo.mode == ECHO_SWITCH);
	engine_set_route(&e, T48AC_ROUTE_HANDSET);
	CHECK(e.echo.mode == ECHO_NONE);
	engine_set_route(&e, 9);	/* nonsense is ignored */
	CHECK(e.route == T48AC_ROUTE_HANDSET);

	T_DONE("engine");
}
