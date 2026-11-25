// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: the per-frame work. See engine.h. */
#include <string.h>
#include "engine.h"

/* Switching a microphone's front end on makes a thump that takes about 0.4 s
 * to die away (measured on the phone: 14000 counts at first). The far end is
 * sent silence for that long after a route is set. */
#define SETTLE_FRAMES 25

void engine_init(struct engine *e)
{
	int i;
	memset(e, 0, sizeof(*e));
	for (i = 0; i < T48AC_VOL_COUNT; i++)
		e->vol[i] = 8;
	e->vol[T48AC_VOL_RINGER] = 13;	/* 4 dB under the loudspeaker's limit */
	e->vol[T48AC_VOL_APP] = 10;
	echo_init(&e->echo, ECHO_NONE, 0);
}

/* The ring melody only ever plays with no route set, which means from the
 * loudspeaker. With a route set, ringing is the call waiting beep. */
static void update_alert(struct engine *e)
{
	static const struct tone_def *const defs[T48AC_TONE_COUNT] = {
		0, &tone_dial, &tone_ringback, &tone_busy, &tone_error, &tone_waiting
	};
	const struct tone_def *want;

	if (e->ring)
		want = e->route ? &tone_waiting : &tone_ring;
	else
		want = defs[e->tone];
	if (want == e->alert_def)
		return;
	e->alert_def = want;
	if (want)
		tone_start(&e->alert, want);
	else
		tone_stop(&e->alert);
}

void engine_set_route(struct engine *e, int route)
{
	if (route < 0 || route >= T48AC_ROUTE_COUNT || route == e->route)
		return;
	e->route = route;
	if (!route)
		e->mute = 0;
	echo_free(&e->echo);
	echo_init(&e->echo, route ? routes[route].echo : ECHO_NONE,
		  route ? routes[route].coupling_q8 : 0);
	memset(&e->dc, 0, sizeof(e->dc));
	memset(&e->lim, 0, sizeof(e->lim));
	e->settle = route ? SETTLE_FRAMES : 0;
	/* a ringer that was sounding must not carry on into the new route */
	if (e->alert_def == &tone_ring || e->alert_def == &tone_waiting) {
		e->alert_def = 0;
		tone_start(&e->alert, 0);
	}
	update_alert(e);
}

void engine_set_volume(struct engine *e, int which, int level)
{
	if (which < 1 || which >= T48AC_VOL_COUNT)
		return;
	e->vol[which] = level < 0 ? 0 : level > T48AC_VOL_MAX ? T48AC_VOL_MAX : level;
}

void engine_set_mute(struct engine *e, int on)
{
	e->mute = e->route ? !!on : 0;
}

void engine_set_tone(struct engine *e, int tone)
{
	if (tone < 0 || tone >= T48AC_TONE_COUNT)
		return;
	e->tone = tone;
	update_alert(e);
}

void engine_set_ring(struct engine *e, int on)
{
	e->ring = !!on;
	update_alert(e);
}

void engine_set_call(struct engine *e, int up)
{
	e->call = !!up;
}

void engine_key(struct engine *e, int key)
{
	tone_start(&e->key, tone_for_key(key));
}

void engine_tick(struct engine *e, const int16_t *mic_in, const int16_t *spk_in,
		 int16_t *mic_out, int16_t *spk_out)
{
	int16_t tbuf[FRAME];
	int vol = e->route ? dsp_vol_q15(e->vol[e->route]) : dsp_alert_q15(e->vol[T48AC_VOL_RINGER]);

	/* What the loudspeaker gets: the call, then the tones on top */
	if (e->route && spk_in) {
		memcpy(spk_out, spk_in, FRAME * sizeof(int16_t));
		dsp_gain(spk_out, FRAME, vol);
	} else {
		/* no call audio, or on hook, where it is not played */
		memset(spk_out, 0, FRAME * sizeof(int16_t));
	}
	if (tone_render(&e->alert, tbuf, FRAME))
		dsp_mix(spk_out, spk_out, tbuf, vol, FRAME);
	if (tone_render(&e->key, tbuf, FRAME))
		dsp_mix(spk_out, spk_out, tbuf, vol, FRAME);
	/* An app's sound, at its own volume: on the loudspeaker a full-scale
	 * sample at 15 is on the limit; in an earpiece it is as loud as a
	 * call. Held (a call, ringing, a tone) it fades out over one frame
	 * and back in after, so nothing clicks. */
	{
		int row = engine_hw_row(e);
		int want = engine_app_held(e) ? 0 :
			routes[row].amp ? dsp_app_q15(e->vol[T48AC_VOL_APP]) : dsp_vol_q15(e->vol[T48AC_VOL_APP]);

		e->app_heard = 0;
		if (e->app) {
			if (want || e->app_gain) {
				dsp_mix_ramp(spk_out, e->app, e->app_gain, want, FRAME);
				e->app_heard = 1;
			}
			e->app = 0;
		}
		e->app_gain = want;
	}
	/* The amplifier can reset the phone (issue #57): call, tones and
	 * ringer all go through the limiter on their way to it */
	if (routes[engine_hw_row(e)].amp)
		dsp_limit(&e->lim, spk_out, FRAME, SPK_LIMIT);

	/* What the call gets: nothing on hook, else the microphone with the
	 * DC taken off and, on the speakerphone, the echo control applied
	 * (it sees the tones too, so a key beep is not sent back as echo) */
	if (!e->route) {
		memset(mic_out, 0, FRAME * sizeof(int16_t));
		return;
	}
	memcpy(mic_out, mic_in, FRAME * sizeof(int16_t));
	dsp_dcblock(&e->dc, mic_out, FRAME);
	echo_process(&e->echo, spk_out, mic_out, FRAME);
	if (e->settle > 0)
		e->settle--;
	if (e->mute || e->settle > 0)
		memset(mic_out, 0, FRAME * sizeof(int16_t));
}
