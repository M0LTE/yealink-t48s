/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48-audiod: what happens to the samples each 20 ms (issue #24). No I/O in
 * here: the daemon feeds it a microphone frame and the call's loudspeaker
 * frame and gets back the frame for the call and the frame to play. That
 * keeps it testable on a PC.
 */
#ifndef AUDIOD_ENGINE_H
#define AUDIOD_ENGINE_H
#include <stdint.h>
#include "dsp.h"
#include "echo.h"
#include "tone.h"
#include "routes.h"

struct engine {
	int route;		/* t48ac_route */
	int mute;
	int tone;		/* t48ac_tone asked for */
	int ring;
	int vol[T48AC_VOL_COUNT];
	const struct tone_def *alert_def;	/* what the alert generator plays now */
	struct tone_gen alert;	/* progress tone or ringer */
	struct tone_gen key;	/* key click or DTMF feedback, mixed on top */
	struct echo echo;
	struct dcblock dc;
	struct limiter lim;	/* on whatever goes to the loudspeaker's amplifier */
	int settle;		/* frames of microphone still to hold back after a route change */
	int call;		/* baresip's call stream is up */
	const int16_t *app;	/* an app's frame for the next tick, or NULL */
	int app_gain;		/* what the app's sound was last mixed at, Q15 */
	int app_heard;		/* the last tick played some of it */
};

void engine_init(struct engine *e);
void engine_set_route(struct engine *e, int route);
void engine_set_volume(struct engine *e, int which, int level);
void engine_set_mute(struct engine *e, int on);
void engine_set_tone(struct engine *e, int tone);
void engine_set_ring(struct engine *e, int on);
void engine_key(struct engine *e, int key);

/* Whether baresip's call stream is up: with a route set, that is a call */
void engine_set_call(struct engine *e, int up);
/* An app's frame (FRAME samples) for the next tick only */
static inline void engine_app_frame(struct engine *e, const int16_t *frame)
{
	e->app = frame;
}

/* Calls win: an app's sound is not played while the phone rings, a tone
 * plays, or a call is up on a route */
static inline int engine_app_held(const struct engine *e)
{
	return e->ring || tone_active(&e->alert) || (e->route && e->call);
}

/* Row of the route table the hardware should be set up for */
static inline int engine_hw_row(const struct engine *e)
{
	return e->route ? e->route : ROUTE_ALERT;
}

/* True while a tone, the ringer or a key sound is playing */
static inline int engine_sounding(const struct engine *e)
{
	return tone_active(&e->alert) || tone_active(&e->key);
}

/* One tick. mic_in: FRAME samples from the hardware. spk_in: the call's
 * loudspeaker frame, or NULL if there is none. mic_out: for the call.
 * spk_out: for the hardware. */
void engine_tick(struct engine *e, const int16_t *mic_in, const int16_t *spk_in,
		 int16_t *mic_out, int16_t *spk_out);

#endif
