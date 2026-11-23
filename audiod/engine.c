// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: the per-frame work. See engine.h. */
#include <string.h>
#include "engine.h"

void engine_init(struct engine *e)
{
	int i;
	memset(e, 0, sizeof(*e));
	for (i = 0; i < T48AC_VOL_COUNT; i++)
		e->vol[i] = 8;
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
	echo_init(&e->echo, route ? routes[route].echo : ECHO_NONE,
		  route ? routes[route].coupling_q8 : 0);
	memset(&e->dc, 0, sizeof(e->dc));
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

void engine_key(struct engine *e, int key)
{
	tone_start(&e->key, tone_for_key(key));
}

void engine_tick(struct engine *e, const int16_t *mic_in, const int16_t *spk_in,
		 int16_t *mic_out, int16_t *spk_out)
{
	int16_t tbuf[FRAME];
	int vol = dsp_vol_q15(e->vol[e->route ? e->route : T48AC_VOL_RINGER]);

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
	if (e->mute)
		memset(mic_out, 0, FRAME * sizeof(int16_t));
}
