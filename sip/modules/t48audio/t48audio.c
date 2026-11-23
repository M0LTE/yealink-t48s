/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48audio: baresip audio source and player for the T48S audio service
 * (t48-audiod, issue #24). Interface stub for issue #25.
 *
 * The shape (see t48audio_proto.h and docs/sip.md):
 *  - One call stream per baresip process, shared by the source ("mic") and
 *    the player ("speaker"). Whichever is opened first opens the stream; it
 *    closes when both are gone.
 *  - The audio service is the clock. Every ptime it hands baresip one mic
 *    frame, and baresip answers with one speaker frame. Both directions run
 *    off the codec's own DMA clock, so there is no drift to correct.
 *  - Always 16 kHz, mono, signed 16-bit. baresip resamples for 8 kHz codecs
 *    (config: ausrc_srate/auplay_srate 16000, channels 1).
 *
 * This stub does not connect to anything yet: a thread stands in for the
 * audio service, ticks every ptime, feeds silence as the mic and throws the
 * speaker audio away (reporting its level once a second at debug level).
 * So it is also the null backend the phone can use before #24 exists.
 * When t48-audiod exists only stream_open/stream_tick change.
 *
 * Config:  audio_player t48audio,call   audio_source t48audio,call
 */
#include <string.h>
#include <re_atomic.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "t48audio_proto.h"

struct ausrc_st {
	ausrc_read_h *rh;
	ausrc_error_h *errh;
	void *arg;
};

struct auplay_st {
	auplay_write_h *wh;
	void *arg;
};

/* The one call stream, shared by source and player */
static struct {
	mtx_t lock;
	thrd_t thread;
	RE_ATOMIC bool run;
	struct ausrc_st *src;     /* NULL when no source is open */
	struct auplay_st *play;   /* NULL when no player is open */
	uint32_t ptime;
	int16_t mic[T48A_SRATE * T48A_MAX_PTIME / 1000];
	int16_t spk[T48A_SRATE * T48A_MAX_PTIME / 1000];
	uint64_t spk_energy;
	uint32_t spk_frames;
} cs;

static struct ausrc *ausrc;
static struct auplay *auplay;


/* One ptime: mic frame to baresip, speaker frame back. With the real
 * service this runs when a T48A_MIC message arrives, and the speaker frame
 * goes back as T48A_SPK. */
static void stream_tick(uint64_t ts_ms)
{
	size_t sampc = T48A_SRATE * cs.ptime / 1000;
	struct auframe af;
	size_t i;

	mtx_lock(&cs.lock);

	if (cs.src) {
		memset(cs.mic, 0, sampc * sizeof(int16_t)); /* stub: silence */
		auframe_init(&af, AUFMT_S16LE, cs.mic, sampc, T48A_SRATE, 1);
		af.timestamp = ts_ms * 1000;
		cs.src->rh(&af, cs.src->arg);
	}

	if (cs.play) {
		auframe_init(&af, AUFMT_S16LE, cs.spk, sampc, T48A_SRATE, 1);
		af.timestamp = ts_ms * 1000;
		cs.play->wh(&af, cs.play->arg);
		for (i = 0; i < sampc; i++)  /* stub: measure, then drop */
			cs.spk_energy += (uint64_t)((int32_t)cs.spk[i] *
						    cs.spk[i]);
		cs.spk_frames += (uint32_t)sampc;
		if (cs.spk_frames >= T48A_SRATE) {
			debug("t48audio: speaker mean square %llu\n",
			      (unsigned long long)(cs.spk_energy /
						   cs.spk_frames));
			cs.spk_energy = 0;
			cs.spk_frames = 0;
		}
	}

	mtx_unlock(&cs.lock);
}


/* Stand-in for the audio service's clock */
static int stream_thread(void *arg)
{
	uint64_t ts = tmr_jiffies();
	(void)arg;

	while (re_atomic_rlx(&cs.run)) {
		int64_t dt = (int64_t)(ts - tmr_jiffies());

		if (dt > 1) {
			sys_msleep((unsigned)(dt > 4 ? 4 : dt));
			continue;
		}
		stream_tick(ts);
		ts += cs.ptime;
	}

	return 0;
}


/* Called with neither src nor play set: start the stream. With the real
 * service: connect to T48A_SOCKET and send T48A_HELLO. */
static int stream_open(uint32_t ptime)
{
	if (re_atomic_rlx(&cs.run))
		return 0;

	if (ptime < 5 || ptime > T48A_MAX_PTIME || ptime % 5) {
		warning("t48audio: ptime %u not supported (5 to %u, steps"
			" of 5)\n", ptime, T48A_MAX_PTIME);
		return ENOTSUP;
	}

	cs.ptime = ptime;
	info("t48audio: call stream open (stub, %u ms frames)\n", ptime);
	re_atomic_rlx_set(&cs.run, true);

	return thread_create_name(&cs.thread, "t48audio", stream_thread,
				  NULL);
}


/* Called once both src and play are gone */
static void stream_close(void)
{
	if (!re_atomic_rlx(&cs.run))
		return;

	re_atomic_rlx_set(&cs.run, false);
	thrd_join(cs.thread, NULL);
	info("t48audio: call stream closed\n");
}


static int check_prm(int fmt, uint32_t srate, uint8_t ch)
{
	if (fmt != AUFMT_S16LE || srate != T48A_SRATE || ch != 1) {
		warning("t48audio: needs s16le %u Hz mono, got %s %u Hz %u ch"
			" (set ausrc_srate/auplay_srate %u and"
			" ausrc_channels/auplay_channels 1)\n",
			T48A_SRATE, aufmt_name(fmt), srate, ch, T48A_SRATE);
		return ENOTSUP;
	}

	return 0;
}


static void src_destructor(void *arg)
{
	struct ausrc_st *st = arg;
	bool last;

	mtx_lock(&cs.lock);
	if (cs.src == st)
		cs.src = NULL;
	last = !cs.src && !cs.play;
	mtx_unlock(&cs.lock);

	if (last)
		stream_close();
}


static void play_destructor(void *arg)
{
	struct auplay_st *st = arg;
	bool last;

	mtx_lock(&cs.lock);
	if (cs.play == st)
		cs.play = NULL;
	last = !cs.src && !cs.play;
	mtx_unlock(&cs.lock);

	if (last)
		stream_close();
}


static int src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		     struct ausrc_prm *prm, const char *dev,
		     ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err;
	(void)as;
	(void)dev;

	if (!stp || !prm || !rh)
		return EINVAL;

	err = check_prm(prm->fmt, prm->srate, prm->ch);
	if (err)
		return err;

	if (re_atomic_rlx(&cs.run) && prm->ptime != cs.ptime)
		return ENOTSUP;

	st = mem_zalloc(sizeof(*st), src_destructor);
	if (!st)
		return ENOMEM;

	st->rh = rh;
	st->errh = errh;
	st->arg = arg;

	err = stream_open(prm->ptime);
	if (err) {
		mem_deref(st);
		return err;
	}

	mtx_lock(&cs.lock);
	if (cs.src) {
		mtx_unlock(&cs.lock);
		warning("t48audio: source already open\n");
		mem_deref(st);
		return EBUSY;
	}
	cs.src = st;
	mtx_unlock(&cs.lock);

	*stp = st;
	return 0;
}


static int play_alloc(struct auplay_st **stp, const struct auplay *ap,
		      struct auplay_prm *prm, const char *dev,
		      auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int err;
	(void)ap;
	(void)dev;

	if (!stp || !prm || !wh)
		return EINVAL;

	err = check_prm(prm->fmt, prm->srate, prm->ch);
	if (err)
		return err;

	if (re_atomic_rlx(&cs.run) && prm->ptime != cs.ptime)
		return ENOTSUP;

	st = mem_zalloc(sizeof(*st), play_destructor);
	if (!st)
		return ENOMEM;

	st->wh = wh;
	st->arg = arg;

	err = stream_open(prm->ptime);
	if (err) {
		mem_deref(st);
		return err;
	}

	mtx_lock(&cs.lock);
	if (cs.play) {
		mtx_unlock(&cs.lock);
		warning("t48audio: player already open\n");
		mem_deref(st);
		return EBUSY;
	}
	cs.play = st;
	mtx_unlock(&cs.lock);

	*stp = st;
	return 0;
}


static int module_init(void)
{
	int err;

	if (mtx_init(&cs.lock, mtx_plain) != thrd_success)
		return ENOMEM;

	err  = ausrc_register(&ausrc, baresip_ausrcl(), "t48audio",
			      src_alloc);
	err |= auplay_register(&auplay, baresip_auplayl(), "t48audio",
			       play_alloc);

	return err;
}


static int module_close(void)
{
	ausrc = mem_deref(ausrc);
	auplay = mem_deref(auplay);
	stream_close();
	mtx_destroy(&cs.lock);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(t48audio) = {
	"t48audio",
	"sound",
	module_init,
	module_close
};
