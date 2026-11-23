/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48audio: baresip audio source and player for the T48S audio service
 * (t48-audiod, issue #24). The boundary is in t48audio_proto.h and
 * docs/audiod.md.
 *
 *  - One call stream per baresip process, shared by the source ("mic") and
 *    the player ("speaker"). Whichever is opened first opens the stream; it
 *    closes when both are gone.
 *  - The audio service is the clock. Every 20 ms it sends one microphone
 *    frame, and this module answers at once with one loudspeaker frame.
 *    Both directions run off the codec's own sample clock, so there is no
 *    drift to correct. Frames are always 20 ms; baresip re-cuts them for
 *    codecs with another packet time.
 *  - Always 16 kHz, mono, signed 16-bit. baresip resamples for 8 kHz codecs
 *    (config: ausrc_srate/auplay_srate 16000, channels 1).
 *  - If the service is not there (not started, or restarting), a call still
 *    works: this module keeps its own 20 ms clock, feeds silence as the
 *    microphone, drops the loudspeaker audio, and tries the socket again
 *    twice a second. So an audio service restart costs a moment of silence,
 *    not the call.
 *
 * Config:  audio_player t48audio,call   audio_source t48audio,call
 *          t48audio_socket /run/t48-audiod/call.sock     (optional)
 */
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <re_atomic.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "t48audio_proto.h"

enum {
	RETRY_MS = 500,       /* between attempts to reach the service */
	STALL_MS = 500,       /* no frame from the service for this long: gone */
};

struct ausrc_st {
	ausrc_read_h *rh;
	ausrc_error_h *errh;
	void *arg;
};

struct auplay_st {
	auplay_write_h *wh;
	void *arg;
};

/* MIC and SPK payload. Little-endian machines only, like the service. */
struct frame_msg {
	uint32_t seq;
	int16_t sampv[T48A_FRAME];
};

/* The one call stream, shared by source and player */
static struct {
	mtx_t lock;
	thrd_t thread;
	RE_ATOMIC bool run;
	struct ausrc_st *src;     /* NULL when no source is open */
	struct auplay_st *play;   /* NULL when no player is open */
	int fd;                   /* the service, or -1; the thread's own */
	char path[108];
	uint64_t frames;          /* since the stream opened, for timestamps */
	bool said_missing;
} cs = { .fd = -1 };

static struct ausrc *ausrc;
static struct auplay *auplay;


/* Read or write exactly len bytes, giving up after STALL_MS without progress */
static int xfer(int fd, void *buf, size_t len, bool wr)
{
	uint8_t *p = buf;

	while (len) {
		struct pollfd pfd = { fd, wr ? POLLOUT : POLLIN, 0 };
		ssize_t n;

		if (poll(&pfd, 1, STALL_MS) <= 0)
			return ETIMEDOUT;
		n = wr ? send(fd, p, len, MSG_NOSIGNAL) : recv(fd, p, len, 0);
		if (n == 0)
			return ECONNRESET;
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return errno;
		}
		p += n;
		len -= (size_t)n;
	}

	return 0;
}


static int msg_send(int fd, uint16_t type, const void *payload, uint16_t len)
{
	uint8_t buf[4 + sizeof(struct frame_msg)];

	if (len > sizeof(struct frame_msg))
		return EMSGSIZE;
	buf[0] = (uint8_t)type;
	buf[1] = (uint8_t)(type >> 8);
	buf[2] = (uint8_t)len;
	buf[3] = (uint8_t)(len >> 8);
	if (len)
		memcpy(buf + 4, payload, len);

	return xfer(fd, buf, 4u + len, true);
}


/* One message. A type whose payload does not fit is read, thrown away and
 * reported as type 0. */
static int msg_recv(int fd, uint16_t *type, void *payload, size_t size,
		    uint16_t *len)
{
	uint8_t h[4], junk[256];
	size_t left;
	int err;

	err = xfer(fd, h, 4, false);
	if (err)
		return err;
	*type = (uint16_t)(h[0] | h[1] << 8);
	*len = (uint16_t)(h[2] | h[3] << 8);
	if (*len <= size)
		return *len ? xfer(fd, payload, *len, false) : 0;

	for (left = *len; left && !err; ) {
		size_t n = left < sizeof(junk) ? left : sizeof(junk);

		err = xfer(fd, junk, n, false);
		left -= n;
	}
	*type = 0;

	return err;
}


static void service_close(const char *why)
{
	if (cs.fd < 0)
		return;
	close(cs.fd);
	cs.fd = -1;
	warning("t48audio: lost the audio service (%s): silence until it"
		" is back\n", why);
}


/* Connect and say hello. Says so once if the service is not there. */
static void service_open(void)
{
	struct sockaddr_un sa;
	uint8_t hello[4] = { T48A_VERSION, 0, T48A_FRAME_MS, 0 }, ready[4];
	uint16_t type = 0, len = 0;
	int fd, err;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	str_ncpy(sa.sun_path, cs.path, sizeof(sa.sun_path));
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		if (!cs.said_missing) {
			cs.said_missing = true;
			warning("t48audio: no audio service at %s (%m):"
				" silence until there is\n", cs.path, errno);
		}
		close(fd);
		return;
	}

	err = msg_send(fd, T48A_HELLO, hello, sizeof(hello));
	if (!err)
		err = msg_recv(fd, &type, ready, sizeof(ready), &len);
	if (err || type != T48A_READY || len < 4 ||
	    ready[0] != T48A_VERSION || ready[2] != T48A_FRAME_MS) {
		if (!cs.said_missing) {
			cs.said_missing = true;
			warning("t48audio: the audio service at %s did not"
				" accept protocol version %d\n", cs.path,
				T48A_VERSION);
		}
		close(fd);
		return;
	}

	cs.fd = fd;
	cs.said_missing = false;
	info("t48audio: call stream connected to %s\n", cs.path);
}


/* One 20 ms frame: the microphone frame to baresip, and the loudspeaker
 * frame baresip gives back into spk. Returns false if no player is open. */
static bool stream_tick(int16_t *mic, int16_t *spk)
{
	struct auframe af;
	bool played = false;

	mtx_lock(&cs.lock);

	if (cs.src) {
		auframe_init(&af, AUFMT_S16LE, mic, T48A_FRAME, T48A_SRATE, 1);
		af.timestamp = cs.frames * T48A_FRAME_MS * 1000;
		cs.src->rh(&af, cs.src->arg);
	}

	if (cs.play) {
		auframe_init(&af, AUFMT_S16LE, spk, T48A_FRAME, T48A_SRATE, 1);
		af.timestamp = cs.frames * T48A_FRAME_MS * 1000;
		cs.play->wh(&af, cs.play->arg);
		played = true;
	}

	cs.frames++;
	mtx_unlock(&cs.lock);

	return played;
}


static int stream_thread(void *arg)
{
	struct frame_msg m, s;
	uint64_t next_try = 0, ts = tmr_jiffies();
	(void)arg;

	while (re_atomic_rlx(&cs.run)) {
		uint16_t type, len;
		int err;

		if (cs.fd < 0) {
			uint64_t now = tmr_jiffies();
			int64_t dt = (int64_t)(ts - now);

			/* No service: our own clock, silence in, audio dropped */
			if (now >= next_try) {
				next_try = now + RETRY_MS;
				service_open();
				if (cs.fd >= 0)
					continue;
			}
			if (dt > 1) {
				sys_msleep((unsigned)(dt > 4 ? 4 : dt));
				continue;
			}
			if (dt < -200)       /* we were held up: do not race */
				ts = now;
			memset(m.sampv, 0, sizeof(m.sampv));
			(void)stream_tick(m.sampv, s.sampv);
			ts += T48A_FRAME_MS;
			continue;
		}

		/* The service's clock: a microphone frame every 20 ms */
		err = msg_recv(cs.fd, &type, &m, sizeof(m), &len);
		if (err) {
			service_close(err == ETIMEDOUT ? "it stopped sending"
				      : "it closed the connection");
			ts = tmr_jiffies();
			next_try = ts + RETRY_MS;
			continue;
		}
		if (type != T48A_MIC || len != T48A_FRAME_LEN)
			continue;

		if (stream_tick(m.sampv, s.sampv)) {
			s.seq = m.seq;
			err = msg_send(cs.fd, T48A_SPK, &s, sizeof(s));
			if (err) {
				service_close("it stopped reading");
				ts = tmr_jiffies();
				next_try = ts + RETRY_MS;
			}
		}
	}

	if (cs.fd >= 0) {
		(void)msg_send(cs.fd, T48A_BYE, NULL, 0);
		close(cs.fd);
		cs.fd = -1;
	}

	return 0;
}


/* Called when the first of source and player opens */
static int stream_open(void)
{
	int err;

	if (re_atomic_rlx(&cs.run))
		return 0;

	cs.frames = 0;
	cs.said_missing = false;
	re_atomic_rlx_set(&cs.run, true);

	err = thread_create_name(&cs.thread, "t48audio", stream_thread, NULL);
	if (err)
		re_atomic_rlx_set(&cs.run, false);

	return err;
}


/* Called once both source and player are gone */
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

	st = mem_zalloc(sizeof(*st), src_destructor);
	if (!st)
		return ENOMEM;

	st->rh = rh;
	st->errh = errh;
	st->arg = arg;

	mtx_lock(&cs.lock);
	if (cs.src) {
		mtx_unlock(&cs.lock);
		warning("t48audio: source already open\n");
		mem_deref(st);
		return EBUSY;
	}
	cs.src = st;
	mtx_unlock(&cs.lock);

	err = stream_open();
	if (err) {
		mem_deref(st);
		return err;
	}

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

	st = mem_zalloc(sizeof(*st), play_destructor);
	if (!st)
		return ENOMEM;

	st->wh = wh;
	st->arg = arg;

	mtx_lock(&cs.lock);
	if (cs.play) {
		mtx_unlock(&cs.lock);
		warning("t48audio: player already open\n");
		mem_deref(st);
		return EBUSY;
	}
	cs.play = st;
	mtx_unlock(&cs.lock);

	err = stream_open();
	if (err) {
		mem_deref(st);
		return err;
	}

	*stp = st;
	return 0;
}


static int module_init(void)
{
	int err;

	if (mtx_init(&cs.lock, mtx_plain) != thrd_success)
		return ENOMEM;

	str_ncpy(cs.path, T48A_SOCKET, sizeof(cs.path));
	(void)conf_get_str(conf_cur(), "t48audio_socket", cs.path,
			   sizeof(cs.path));

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
