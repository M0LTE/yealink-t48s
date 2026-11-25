/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48audio: baresip audio source and player for the T48S audio service
 * (t48-audiod, issue #24). The boundary is in t48audio_proto.h and
 * docs/audiod.md.
 *
 *  - One call stream per baresip process, shared by every source ("mic")
 *    and player ("speaker"). The first to open opens the stream; it closes
 *    when all are gone. There is one of each per call, and the phone can
 *    have two calls (one on hold, or one being transferred): each source
 *    gets the microphone frame, and the players' frames are added together.
 *    A call on hold sends and plays nothing, so in practice one is heard.
 *  - The audio service is the clock. Every 20 ms it sends one microphone
 *    frame, and this module answers at once with one loudspeaker frame.
 *    Both directions run off the codec's own sample clock, so there is no
 *    drift to correct. Frames are always 20 ms; baresip re-cuts them for
 *    codecs with another packet time.
 *  - Always 16 kHz, mono, signed 16-bit. baresip resamples for 8 kHz codecs
 *    (config: ausrc_srate/auplay_srate 16000, channels 1).
 *  - In-band DTMF (issue #77): for an account whose DTMF is "inband" the
 *    shell sends the command t48inband with the digits, and each becomes
 *    100 ms of its two tones in place of the microphone, then 60 ms of
 *    the microphone again, before the next. baresip has no in-band mode of
 *    its own; RFC 2833 and SIP INFO are its dtmfmode.
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
#include <math.h>
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
	MAX_CALLS = 4,        /* sources, and players, at once */
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
	struct ausrc_st *src[MAX_CALLS];     /* NULL where none is open */
	struct auplay_st *play[MAX_CALLS];
	int fd;                   /* the service, or -1; the thread's own */
	char path[108];
	uint64_t frames;          /* since the stream opened, for timestamps */
	bool said_missing;
} cs = { .fd = -1 };

static struct ausrc *ausrc;
static struct auplay *auplay;

/* In-band DTMF: digits waiting, and how far into the first one we are */
enum {
	DTMF_QUEUE = 32,
	DTMF_ON = T48A_SRATE * 100 / 1000,    /* samples of tone */
	DTMF_GAP = T48A_SRATE * 60 / 1000,    /* then of the microphone */
};
static struct {
	char q[DTMF_QUEUE];
	size_t n;
	uint32_t pos;         /* samples into q[0]'s tone and gap */
} dtmf;                   /* under cs.lock */

/* one sine cycle in 1024 steps, made once: the phone has no FPU, so a
 * tone is a table lookup a sample, not a sin() */
static int16_t sine[1024];


/* the two frequencies of a key, or false if it is not one */
static bool dtmf_freqs(char key, uint32_t *lo, uint32_t *hi)
{
	static const char keys[] = "123A456B789C*0#D";
	static const uint32_t row[] = { 697, 770, 852, 941 };
	static const uint32_t col[] = { 1209, 1336, 1477, 1633 };
	const char *p = key ? strchr(keys, key) : NULL;

	if (!p)
		return false;
	*lo = row[(p - keys) / 4];
	*hi = col[(p - keys) % 4];
	return true;
}


/* With the lock held: the next 20 ms of the digit being sent into frame,
 * in place of the microphone while its tone lasts. */
static void dtmf_frame(int16_t *frame)
{
	uint32_t lo, hi;
	size_t k;

	if (!dtmf.n)
		return;
	if (dtmf_freqs(dtmf.q[0], &lo, &hi)) {
		for (k = 0; k < T48A_FRAME; k++) {
			uint32_t t = dtmf.pos + (uint32_t)k;

			if (t >= DTMF_ON)
				break;
			/* the phase, in 1024ths of a cycle; high group 2 dB
			 * over the low, about -9 dBFS together */
			frame[k] = (int16_t)((7000 * sine[(lo * t * 1024 / T48A_SRATE) & 1023] +
					      8800 * sine[(hi * t * 1024 / T48A_SRATE) & 1023]) >> 15);
		}
	}
	dtmf.pos += T48A_FRAME;
	if (dtmf.pos >= DTMF_ON + DTMF_GAP) {
		memmove(dtmf.q, dtmf.q + 1, --dtmf.n);
		dtmf.pos = 0;
	}
}


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
	int16_t one[T48A_FRAME], in[T48A_FRAME];
	bool played = false;
	size_t i, k;

	mtx_lock(&cs.lock);

	dtmf_frame(mic);
	for (i = 0; i < MAX_CALLS; i++) {
		if (!cs.src[i])
			continue;
		/* a source may change the frame it is given: each its own */
		memcpy(in, mic, sizeof(in));
		auframe_init(&af, AUFMT_S16LE, in, T48A_FRAME, T48A_SRATE, 1);
		af.timestamp = cs.frames * T48A_FRAME_MS * 1000;
		cs.src[i]->rh(&af, cs.src[i]->arg);
	}

	for (i = 0; i < MAX_CALLS; i++) {
		if (!cs.play[i])
			continue;
		memset(one, 0, sizeof(one));
		auframe_init(&af, AUFMT_S16LE, played ? one : spk, T48A_FRAME,
			     T48A_SRATE, 1);
		af.timestamp = cs.frames * T48A_FRAME_MS * 1000;
		cs.play[i]->wh(&af, cs.play[i]->arg);
		if (played) {
			for (k = 0; k < T48A_FRAME; k++) {
				int32_t v = spk[k] + one[k];

				spk[k] = (int16_t)(v > 32767 ? 32767 :
						   v < -32768 ? -32768 : v);
			}
		}
		played = true;
	}

	cs.frames++;
	mtx_unlock(&cs.lock);

	return played;
}


/* with the lock held */
static bool stream_idle(void)
{
	size_t i;

	for (i = 0; i < MAX_CALLS; i++)
		if (cs.src[i] || cs.play[i])
			return false;
	return true;
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
	mtx_lock(&cs.lock);
	dtmf.n = 0;           /* nothing left over from the last call */
	dtmf.pos = 0;
	mtx_unlock(&cs.lock);
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

	size_t i;

	mtx_lock(&cs.lock);
	for (i = 0; i < MAX_CALLS; i++)
		if (cs.src[i] == st)
			cs.src[i] = NULL;
	last = stream_idle();
	mtx_unlock(&cs.lock);

	if (last)
		stream_close();
}


static void play_destructor(void *arg)
{
	struct auplay_st *st = arg;
	bool last;

	size_t i;

	mtx_lock(&cs.lock);
	for (i = 0; i < MAX_CALLS; i++)
		if (cs.play[i] == st)
			cs.play[i] = NULL;
	last = stream_idle();
	mtx_unlock(&cs.lock);

	if (last)
		stream_close();
}


static int src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		     struct ausrc_prm *prm, const char *dev,
		     ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	size_t i;
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
	for (i = 0; i < MAX_CALLS && cs.src[i]; i++)
		;
	if (i == MAX_CALLS) {
		mtx_unlock(&cs.lock);
		warning("t48audio: too many sources open\n");
		mem_deref(st);
		return EBUSY;
	}
	cs.src[i] = st;
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
	size_t i;
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
	for (i = 0; i < MAX_CALLS && cs.play[i]; i++)
		;
	if (i == MAX_CALLS) {
		mtx_unlock(&cs.lock);
		warning("t48audio: too many players open\n");
		mem_deref(st);
		return EBUSY;
	}
	cs.play[i] = st;
	mtx_unlock(&cs.lock);

	err = stream_open();
	if (err) {
		mem_deref(st);
		return err;
	}

	*stp = st;
	return 0;
}


/* t48inband DIGITS: queue them as tones in the call's audio */
static int cmd_inband(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	const char *p;
	uint32_t lo, hi;
	(void)pf;

	if (!carg->prm || !*carg->prm)
		return EINVAL;
	mtx_lock(&cs.lock);
	for (p = carg->prm; *p; p++) {
		if (!dtmf_freqs(*p, &lo, &hi) || dtmf.n >= DTMF_QUEUE)
			continue;
		dtmf.q[dtmf.n++] = *p;
		info("t48audio: in-band DTMF '%c'\n", *p);
	}
	mtx_unlock(&cs.lock);

	return 0;
}


static const struct cmd cmdv[] = {
	{"t48inband", 0, CMD_PRM, "Send DTMF as tones in the audio",
	 cmd_inband},
};


static int module_init(void)
{
	int err;

	if (mtx_init(&cs.lock, mtx_plain) != thrd_success)
		return ENOMEM;

	for (size_t i = 0; i < RE_ARRAY_SIZE(sine); i++)
		sine[i] = (int16_t)(32767 * sin(2 * M_PI * (double)i / RE_ARRAY_SIZE(sine)));

	str_ncpy(cs.path, T48A_SOCKET, sizeof(cs.path));
	(void)conf_get_str(conf_cur(), "t48audio_socket", cs.path,
			   sizeof(cs.path));

	err  = ausrc_register(&ausrc, baresip_ausrcl(), "t48audio",
			      src_alloc);
	err |= auplay_register(&auplay, baresip_auplayl(), "t48audio",
			       play_alloc);
	err |= cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));

	return err;
}


static int module_close(void)
{
	cmd_unregister(baresip_commands(), cmdv);
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
