// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-audiod: the one program that owns the T48S's audio hardware
 * (issue #24, docs/audiod.md).
 *
 *   t48-audiod [--backend null|alsa] [--mic IN.wav] [--spk OUT.wav] [--room MS,PERCENT]
 *              [--run-dir DIR] [--routes FILE] [--card N] [--print-routes] [-v]
 *
 *   --backend    null (the default: no hardware, a timer for a clock) or alsa
 *                (the phone). --mic or --spk turn null into the file backend.
 *   --run-dir    where the two sockets go (default /run/t48-audiod):
 *                call.sock for baresip, ctl.sock for the shell
 *   --routes     a routes file to lay over the built-in table
 *   --room MS,PERCENT   file backend, for tests: what is played comes back
 *                into the microphone MS later at PERCENT of its level
 *
 * One thread, one poll loop. While nothing is wanted (no route, no call
 * stream, no tone) the hardware is closed and the process sleeps in poll.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "appsnd.h"
#include "engine.h"
#include "hw.h"
#include "msg.h"
#include "pace.h"
#include "t48audio_proto.h"

#define MAX_CTL      4
#define LINGER_TICKS 5	/* run on 100 ms after the last sound, so it is played out */
/* After an app's sound the hardware is kept for 2 s: a game's next beep then
 * starts at once and the amplifier is not switched for each one */
#define APP_LINGER_TICKS 100
#define APP_HEARD_TICKS  25	/* STATE says an app is sounding for this long after */

struct client {
	int fd;
	int passed;	/* a descriptor that came with a message, or -1 */
	struct msgbuf mb;
};

static struct engine eng;
static struct fq spkq;
static struct hw *hw;
static int hw_running, hw_row = -1, linger, verbose;
static int64_t hw_retry_at;
static struct client call = { .fd = -1 };
static struct appsnd app;
static int app_told, app_owner = -1, app_heard;

/* --record: a test aid for measuring and tuning on the phone. While a call
 * stream is open, each tick's three frames go to a 3-channel WAV file:
 * the microphone as sent to baresip, the far end as received from baresip,
 * and the loudspeaker as played. It stops by itself after REC_MAX_TICKS. */
#define REC_MAX_TICKS (120 * 50)
static FILE *rec;
static unsigned long rec_ticks;

static void rec_header(void)
{
	uint32_t bytes = (uint32_t)(rec_ticks * FRAME * 6);
	uint32_t h[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16,
			   0x00030001, SRATE, SRATE * 6, 0x00100006, 0x61746164, bytes };

	fseek(rec, 0, SEEK_SET);
	fwrite(h, 4, 11, rec);	/* little-endian hosts only, like the call stream */
	fflush(rec);
}

static void rec_frame(const int16_t *mic, const int16_t *far, const int16_t *spk)
{
	int16_t out[FRAME * 3];
	int i;

	if (!rec || rec_ticks >= REC_MAX_TICKS)
		return;
	for (i = 0; i < FRAME; i++) {
		out[3 * i] = mic[i];
		out[3 * i + 1] = far ? far[i] : 0;
		out[3 * i + 2] = spk[i];
	}
	fseek(rec, 44 + (long)rec_ticks * FRAME * 6, SEEK_SET);
	fwrite(out, 2, FRAME * 3, rec);
	rec_ticks++;
}
static int call_ready;
static uint32_t mic_seq;
static struct client ctl[MAX_CTL];
static int ctl_wants_state[MAX_CTL];
static struct t48ac_state last_state;

/* per call stream, for the log */
static struct {
	unsigned long ticks, spk_in, late;
	unsigned long tick_us_sum, tick_us_max;
} st;

static void say(const char *fmt, ...)
{
	va_list ap;
	fputs("t48-audiod: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int listen_unix(const char *dir, const char *name)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/%s", dir, name) >= (int)sizeof(sa.sun_path)) {
		say("%s/%s: path too long", dir, name);
		exit(1);
	}
	unlink(sa.sun_path);
	/* umask 077 is set, so the socket is born 0600 */
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 4) < 0) {
		say("%s: %s", sa.sun_path, strerror(errno));
		exit(1);
	}
	return fd;
}

static void call_drop(const char *why)
{
	if (call.fd < 0)
		return;
	close(call.fd);
	call.fd = -1;
	call_ready = 0;
	say("call stream closed (%s): %lu frames sent, %lu received, %lu late, %lu silent ticks, "
	    "%lu trimmed, %lu overflowed; tick mean %lu us, max %lu us; hardware overruns %lu, "
	    "underruns %lu, dropped %lu",
	    why, st.ticks, st.spk_in, st.late, (unsigned long)spkq.underruns,
	    (unsigned long)spkq.trimmed, (unsigned long)spkq.overflowed,
	    st.ticks ? st.tick_us_sum / st.ticks : 0, st.tick_us_max,
	    hw->cap_xruns, hw->play_xruns, hw->play_dropped);
}

static void call_accept(int lfd)
{
	int fd = accept4(lfd, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);

	if (fd < 0)
		return;
	call_drop("replaced by a new connection");
	call.fd = fd;
	call.mb.len = call.mb.used = 0;
	mic_seq = 0;
	memset(&st, 0, sizeof(st));
	fq_reset(&spkq);
}

static void call_read(void)
{
	const unsigned char *p;
	unsigned type, len;
	int r;

	if (msg_fill(call.fd, &call.mb) < 0) {
		call_drop("baresip closed it");
		return;
	}
	while ((r = msg_next(&call.mb, &type, &p, &len)) == 1) {
		if (type == T48A_HELLO) {
			unsigned char ready[4] = { T48A_VERSION, 0, T48A_FRAME_MS, 0 };
			unsigned ver = len >= 2 ? p[0] | p[1] << 8 : 0;
			unsigned ms = len >= 4 ? p[2] | p[3] << 8 : 0;

			if (ver != T48A_VERSION || ms != T48A_FRAME_MS) {
				say("call stream: version %u, %u ms frames: not served (want %u, %u)",
				    ver, ms, T48A_VERSION, T48A_FRAME_MS);
				call_drop("refused");
				return;
			}
			if (msg_send(call.fd, T48A_READY, ready, 4) < 0) {
				call_drop("send failed");
				return;
			}
			call_ready = 1;
			say("call stream open");
		} else if (type == T48A_SPK && len == T48A_FRAME_LEN && call_ready) {
			int16_t frame[FRAME];
			uint32_t seq = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;

			memcpy(frame, p + 4, sizeof(frame));	/* little-endian hosts only */
			fq_push(&spkq, frame);
			st.spk_in++;
			if (seq + 1 != mic_seq)
				st.late++;
		} else if (type == T48A_BYE) {
			call_drop("bye");
			return;
		}
	}
	if (r < 0)
		call_drop("bad message");
}

static void fill_state(struct t48ac_state *s)
{
	int i;

	memset(s, 0, sizeof(*s));
	s->version = T48AC_VERSION;
	s->route = (unsigned char)eng.route;
	s->mute = (unsigned char)eng.mute;
	s->tone = (unsigned char)eng.tone;
	s->ring = (unsigned char)eng.ring;
	s->call = (unsigned char)call_ready;
	for (i = 1; i <= T48AC_VOL_RINGER; i++)
		s->vol[i] = (unsigned char)eng.vol[i];
	s->hw = (unsigned char)(hw->caps | (hw_running ? T48AC_HW_RUNNING : 0));
	s->vol_app = (unsigned char)eng.vol[T48AC_VOL_APP];
	s->app = (unsigned char)app_told;
	s->app_sound = app_heard > 0;
}

/* An app's sound, as the log wants it when the app goes */
static void app_drop(const char *why)
{
	if (!app.ring)
		return;
	say("app sound closed (%s): %lu samples in %lu frames, peak %d, ran dry %lu times, "
	    "%lu resets; hardware underruns %lu, dropped %lu",
	    why, app.samples, app.frames, app.peak, app.dry, app.resets,
	    hw->play_xruns, hw->play_dropped);
	appsnd_close(&app);
	app_told = T48AC_APP_GONE;
	app_owner = -1;
}

static void ctl_drop(int i)
{
	/* an app does not outlive the shell that started it */
	if (app_owner == i)
		app_drop("the shell went away");
	if (ctl[i].passed >= 0)
		close(ctl[i].passed);
	ctl[i].passed = -1;
	close(ctl[i].fd);
	ctl[i].fd = -1;
}

static void ctl_error(int i, unsigned type, unsigned reason)
{
	unsigned char e[4] = { (unsigned char)type, (unsigned char)(type >> 8),
			       (unsigned char)reason, 0 };
	if (msg_send(ctl[i].fd, T48AC_ERROR, e, 4) < 0)
		ctl_drop(i);
}

static void ctl_read(int i)
{
	const unsigned char *p;
	unsigned type, len;
	int r;

	if (msg_fill_fd(ctl[i].fd, &ctl[i].mb, &ctl[i].passed) < 0) {
		ctl_drop(i);
		return;
	}
	while (ctl[i].fd >= 0 && (r = msg_next(&ctl[i].mb, &type, &p, &len)) == 1) {
		static const unsigned char need[] = {
			[T48AC_HELLO] = 2, [T48AC_ROUTE] = 1, [T48AC_VOLUME] = 2, [T48AC_MUTE] = 1,
			[T48AC_TONE] = 1, [T48AC_RING] = 1, [T48AC_KEY] = 1, [T48AC_GET] = 0,
			[T48AC_APP] = 1,
		};

		if (type < T48AC_HELLO || type > T48AC_APP)
			continue;	/* unknown: skipped, as the protocol says */
		if (len < need[type]) {
			ctl_error(i, type, T48AC_ERR_LENGTH);
			continue;
		}
		if (verbose)
			say("control: type %u value %u %u", type, len ? p[0] : 0, len > 1 ? p[1] : 0);
		switch (type) {
		case T48AC_HELLO:
			if ((p[0] | p[1] << 8) != T48AC_VERSION) {
				ctl_error(i, type, T48AC_ERR_VERSION);
				break;
			}
			ctl_wants_state[i] = 1;
			break;
		case T48AC_GET:
			ctl_wants_state[i] = 1;
			break;
		case T48AC_ROUTE:
			if (p[0] >= T48AC_ROUTE_COUNT) {
				ctl_error(i, type, T48AC_ERR_VALUE);
				break;
			}
			if (p[0] != eng.route)
				say("route %s", p[0] ? routes[p[0]].name : "none");
			engine_set_route(&eng, p[0]);
			break;
		case T48AC_VOLUME:
			if (p[0] < 1 || p[0] >= T48AC_VOL_COUNT || p[1] > T48AC_VOL_MAX) {
				ctl_error(i, type, T48AC_ERR_VALUE);
				break;
			}
			engine_set_volume(&eng, p[0], p[1]);
			break;
		case T48AC_MUTE:
			engine_set_mute(&eng, p[0]);
			break;
		case T48AC_TONE:
			if (p[0] >= T48AC_TONE_COUNT) {
				ctl_error(i, type, T48AC_ERR_VALUE);
				break;
			}
			engine_set_tone(&eng, p[0]);
			break;
		case T48AC_RING:
			engine_set_ring(&eng, p[0]);
			break;
		case T48AC_KEY:
			engine_key(&eng, p[0]);
			break;
		case T48AC_APP:
			if (p[0] >= T48AC_APP_COUNT) {
				ctl_error(i, type, T48AC_ERR_VALUE);
				break;
			}
			if (p[0] == T48AC_APP_GONE) {
				if (app_owner == i)
					app_drop("the app has gone");
				break;
			}
			if (ctl[i].passed >= 0) {
				int fd = ctl[i].passed;

				ctl[i].passed = -1;
				app_drop("replaced by a new app");
				if (appsnd_open(&app, fd) < 0) {
					say("app sound: the ring is not a sealed memfd of %d bytes: refused", T48AC_APP_BYTES);
					ctl_error(i, type, T48AC_ERR_RING);
					break;
				}
				app_owner = i;
				say("app sound open");
			}
			if (!app.ring || app_owner != i) {
				ctl_error(i, type, T48AC_ERR_RING);
				break;
			}
			app_told = p[0];
			appsnd_front(&app, p[0] == T48AC_APP_FRONT);
			appsnd_publish(&app, engine_app_held(&eng), hw->play_delay);
			break;
		}
	}
	if (ctl[i].fd >= 0 && r < 0)
		ctl_drop(i);
}

static void ctl_accept(int lfd)
{
	int fd = accept4(lfd, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC), i;

	if (fd < 0)
		return;
	for (i = 0; i < MAX_CTL; i++)
		if (ctl[i].fd < 0)
			break;
	if (i == MAX_CTL) {
		close(fd);
		return;
	}
	ctl[i].fd = fd;
	ctl[i].passed = -1;
	ctl[i].mb.len = ctl[i].mb.used = 0;
	ctl_wants_state[i] = 0;
}

/* Send STATE to whoever asked, and to everyone if it changed */
static void ctl_publish(void)
{
	struct t48ac_state s;
	int i, changed;

	fill_state(&s);
	changed = memcmp(&s, &last_state, sizeof(s)) != 0;
	last_state = s;
	for (i = 0; i < MAX_CTL; i++) {
		if (ctl[i].fd < 0 || !(changed || ctl_wants_state[i]))
			continue;
		ctl_wants_state[i] = 0;
		if (msg_send(ctl[i].fd, T48AC_STATE, &s, sizeof(s)) < 0)
			ctl_drop(i);
	}
}

static int wanted(void)
{
	return eng.route || call.fd >= 0 || engine_sounding(&eng) || appsnd_waiting(&app) > 0;
}

/* Open, switch or close the hardware to match what is wanted */
static void manage_hw(void)
{
	int row = engine_hw_row(&eng);

	if (hw_running && !wanted() && linger <= 0) {
		hw->stop(hw);
		hw_running = 0;
		say("hardware idle");
	} else if (wanted() && (!hw_running || row != hw_row)) {
		if (now_us() < hw_retry_at)
			return;
		if (hw->start(hw, &routes[row]) < 0) {
			hw->stop(hw);
			hw_running = 0;
			linger = 0;
			hw_retry_at = now_us() + 1000000;
			return;
		}
		if (verbose || !hw_running)
			say("hardware running (%s), set for %s%s", hw->name, routes[row].name,
			    routes[row].confirmed ? "" : " (route not confirmed on a phone)");
		hw_running = 1;
		hw_row = row;
	}
}

static void tick(const int16_t *mic)
{
	int16_t spk_in[FRAME], spk_out[FRAME], app_in[FRAME];
	struct { uint32_t seq; int16_t samples[FRAME]; } mic_msg;	/* little-endian hosts only */
	int64_t t0 = now_us(), dt;
	int have = call_ready && fq_pop(&spkq, spk_in);

	engine_set_call(&eng, call_ready);
	if (appsnd_take(&app, app_in, engine_app_held(&eng), hw->play_delay))
		engine_app_frame(&eng, app_in);
	engine_tick(&eng, mic, have ? spk_in : 0, mic_msg.samples, spk_out);
	hw->playback(hw, spk_out);
	if (call_ready || app.front)
		rec_frame(mic_msg.samples, have ? spk_in : 0, spk_out);
	if (call_ready) {
		mic_msg.seq = mic_seq++;
		if (msg_send(call.fd, T48A_MIC, &mic_msg, sizeof(mic_msg)) < 0)
			call_drop("baresip stopped reading");
	}
	if (!wanted()) {
		if (linger > 0)
			linger--;
	} else if (linger < LINGER_TICKS) {
		linger = LINGER_TICKS;
	}
	if (app_heard > 0)
		app_heard--;
	if (eng.app_heard) {
		linger = APP_LINGER_TICKS;
		app_heard = APP_HEARD_TICKS;
	}
	dt = now_us() - t0;
	st.ticks++;
	st.tick_us_sum += (unsigned long)dt;
	if ((unsigned long)dt > st.tick_us_max)
		st.tick_us_max = (unsigned long)dt;
}

int main(int argc, char **argv)
{
	const char *backend = "null", *mic = 0, *spk = 0, *run_dir = "/run/t48-audiod", *routes_file = 0;
	const char *rec_file = 0;
	int card = 0, print_routes = 0, room_ms = 0, room_percent = 0, i, lctl, lcall, sfd;
	sigset_t sigs;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : 0;
		if (!strcmp(a, "-v")) verbose = 1;
		else if (!strcmp(a, "--print-routes")) print_routes = 1;
		else if (!strcmp(a, "--backend") && v) backend = v, i++;
		else if (!strcmp(a, "--mic") && v) mic = v, i++;
		else if (!strcmp(a, "--spk") && v) spk = v, i++;
		else if (!strcmp(a, "--run-dir") && v) run_dir = v, i++;
		else if (!strcmp(a, "--routes") && v) routes_file = v, i++;
		else if (!strcmp(a, "--record") && v) rec_file = v, i++;
		else if (!strcmp(a, "--card") && v) card = atoi(v), i++;
		else if (!strcmp(a, "--room") && v && sscanf(v, "%d,%d", &room_ms, &room_percent) == 2) i++;
		else {
			fprintf(stderr, "usage: t48-audiod [--backend null|alsa] [--mic IN.wav] [--spk OUT.wav] [--room MS,PERCENT]\n"
				"                  [--run-dir DIR] [--routes FILE] [--card N] [--print-routes] [-v]\n"
				"                  [--record CALL.wav]\n");
			return 2;
		}
	}
	if (routes_file && routes_load(routes_file) < 0)
		return 1;
	routes_fix();
	if (print_routes) {
		routes_print();
		return 0;
	}
	if (!strcmp(backend, "alsa")) {
		/* On the phone nothing else may hold the sound up: one SSH login or a
		 * busy app is enough to make it 40 ms late, which is heard (measured).
		 * The work is about 0.6 ms in every 20. */
		struct sched_param prio = { .sched_priority = 10 };

		/* the system call itself: musl's sched_setscheduler() refuses to work */
		if (syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &prio) < 0 ||
		    mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
			say("no real-time priority (%s): sound may break up when the phone is busy", strerror(errno));
		hw = hw_alsa_new(card);
	}
	else if (!strcmp(backend, "null"))
		hw = hw_null_new(mic, spk, room_ms, room_percent);
	else
		say("unknown backend %s", backend);
	if (!hw)
		return 1;

	umask(077);
	if (rec_file && !(rec = fopen(rec_file, "wb")))
		say("%s: %s", rec_file, strerror(errno));
	if (rec) {
		rec_header();
		say("RECORDING the call stream to %s (test aid, at most %d s)", rec_file, REC_MAX_TICKS / 50);
	}

	engine_init(&eng);
	fq_reset(&spkq);
	for (i = 0; i < MAX_CTL; i++)
		ctl[i].fd = ctl[i].passed = -1;
	signal(SIGPIPE, SIG_IGN);
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGINT);
	sigprocmask(SIG_BLOCK, &sigs, 0);
	sfd = signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC);

	umask(077);
	if (mkdir(run_dir, 0700) < 0 && errno != EEXIST) {
		say("%s: %s", run_dir, strerror(errno));
		return 1;
	}
	lcall = listen_unix(run_dir, "call.sock");
	lctl = listen_unix(run_dir, "ctl.sock");
	fill_state(&last_state);
	say("ready: backend %s, sockets in %s", hw->name, run_dir);

	for (;;) {
		struct pollfd p[5 + MAX_CTL];
		int n = 0, itick = -1, icall = -1, ictl[MAX_CTL], timeout = -1, r;

		manage_hw();
		ctl_publish();
		p[n++] = (struct pollfd){ sfd, POLLIN, 0 };
		p[n++] = (struct pollfd){ lcall, POLLIN, 0 };
		p[n++] = (struct pollfd){ lctl, POLLIN, 0 };
		if (hw_running) {
			itick = n;
			p[n++] = (struct pollfd){ hw->tick_fd(hw), POLLIN, 0 };
		} else if (wanted()) {
			timeout = 200;	/* the hardware would not start: try again soon */
		} else if (app.front) {
			/* nothing tells us when an app starts to write: look
			 * every 20 ms while one with sound is on the screen */
			timeout = FRAME_MS;
			appsnd_publish(&app, engine_app_held(&eng), hw->play_delay);
		}
		if (call.fd >= 0) {
			icall = n;
			p[n++] = (struct pollfd){ call.fd, POLLIN, 0 };
		}
		for (i = 0; i < MAX_CTL; i++) {
			ictl[i] = -1;
			if (ctl[i].fd >= 0) {
				ictl[i] = n;
				p[n++] = (struct pollfd){ ctl[i].fd, POLLIN, 0 };
			}
		}
		if (poll(p, (nfds_t)n, timeout) < 0 && errno != EINTR)
			break;
		if (p[0].revents)
			break;
		/* The loudspeaker frames first, so this tick can play them */
		if (icall >= 0 && p[icall].revents)
			call_read();
		if (itick >= 0 && p[itick].revents) {
			int16_t micbuf[FRAME];

			while ((r = hw->capture(hw, micbuf)) == 1)
				tick(micbuf);
			if (r < 0) {
				say("hardware failed: restarting it");
				hw->stop(hw);
				hw_running = 0;
				linger = 0;
				hw_retry_at = now_us() + 200000;
			}
		}
		for (i = 0; i < MAX_CTL; i++)
			if (ictl[i] >= 0 && p[ictl[i]].revents && ctl[i].fd >= 0)
				ctl_read(i);
		if (p[1].revents)
			call_accept(lcall);
		if (p[2].revents)
			ctl_accept(lctl);
	}

	call_drop("service stopping");
	app_drop("service stopping");
	if (rec) {
		rec_header();
		fclose(rec);
	}
	hw->stop(hw);
	hw->destroy(hw);
	say("stopped");
	return 0;
}
