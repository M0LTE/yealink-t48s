// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-audioctl: talk to t48-audiod's control socket from a shell prompt or a
 * test. It speaks the same protocol the phone's shell will
 * (include/t48_audio_ctl.h).
 *
 *   t48-audioctl [-s SOCKET] COMMAND...
 *     route none|handset|speaker|headset
 *     volume handset|speaker|headset|ringer|app 0..15
 *     mute 0|1        tone off|dial|ringback|busy|error|waiting
 *     ring 0|1|standard|long|chime|pulse    (a ringtone's name rings with it)
 *     key CHAR
 *     state           (the default) print the state
 * Several commands can be given in one go. It always ends by printing the
 * state on one line.
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "include/t48_audio_ctl.h"
#include "msg.h"

static const char *const route_names[] = { "none", "handset", "speaker", "headset" };
static const char *const vol_names[] = { "", "handset", "speaker", "headset", "ringer", "app" };
static const char *const app_names[] = { "none", "paused", "front" };
static const char *const tone_names[] = { "off", "dial", "ringback", "busy", "error", "waiting" };
static const char *const ringtone_names[] = { "standard", "long", "chime", "pulse", "custom" };

static int lookup(const char *const *names, int n, const char *s)
{
	int i;
	for (i = 0; i < n; i++)
		if (*names[i] && !strcmp(names[i], s))
			return i;
	fprintf(stderr, "t48-audioctl: \"%s\" not understood\n", s);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *path = T48AC_SOCKET;
	struct sockaddr_un sa;
	struct msgbuf mb = { .len = 0 };
	struct t48ac_state s;
	unsigned char b[2] = { T48AC_VERSION, 0 };
	int fd, i = 1, got = 0, failed = 0;

	if (argc > 2 && !strcmp(argv[1], "-s")) {
		path = argv[2];
		i = 3;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror(path);
		return 1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	msg_send(fd, T48AC_HELLO, b, 2);
	for (; i < argc; i++) {
		const char *c = argv[i], *a1 = i + 1 < argc ? argv[i + 1] : "", *a2 = i + 2 < argc ? argv[i + 2] : "";
		if (!strcmp(c, "state")) {
			continue;
		} else if (!strcmp(c, "route")) {
			b[0] = (unsigned char)lookup(route_names, 4, a1);
			msg_send(fd, T48AC_ROUTE, b, 1);
		} else if (!strcmp(c, "volume")) {
			b[0] = (unsigned char)lookup(vol_names, 6, a1);
			b[1] = (unsigned char)atoi(a2);
			msg_send(fd, T48AC_VOLUME, b, 2);
			i++;
		} else if (!strcmp(c, "mute")) {
			b[0] = (unsigned char)atoi(a1);
			msg_send(fd, T48AC_MUTE, b, 1);
		} else if (!strcmp(c, "tone")) {
			b[0] = (unsigned char)lookup(tone_names, 6, a1);
			msg_send(fd, T48AC_TONE, b, 1);
		} else if (!strcmp(c, "ring")) {
			int t;
			for (t = 0; t < 4 && strcmp(a1, ringtone_names[t]); t++)
				;
			b[0] = t < 4 ? 1 : (unsigned char)atoi(a1);
			b[1] = (unsigned char)(t < 4 ? t : 0);
			msg_send(fd, T48AC_RING, b, 2);
		} else if (!strcmp(c, "key")) {
			b[0] = (unsigned char)a1[0];
			msg_send(fd, T48AC_KEY, b, 1);
		} else {
			fprintf(stderr, "t48-audioctl: \"%s\" not understood\n", c);
			return 2;
		}
		i++;
	}
	msg_send(fd, T48AC_GET, 0, 0);

	/* Take what comes until it has been quiet for a moment; the last
	 * STATE is the one that counts */
	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		const unsigned char *pl;
		unsigned type, len;

		if (poll(&p, 1, got ? 150 : 2000) <= 0)
			break;
		if (msg_fill(fd, &mb) < 0)
			break;
		while (msg_next(&mb, &type, &pl, &len) == 1) {
			if (type == T48AC_STATE && len >= 8) {
				/* an older service sends fewer bytes */
				memset(&s, 0, sizeof(s));
				memcpy(&s, pl, len < sizeof(s) ? len : sizeof(s));
				got = 1;
			} else if (type == T48AC_ERROR && len >= 4) {
				fprintf(stderr, "t48-audioctl: message type %u refused, reason %u\n",
					pl[0] | pl[1] << 8, pl[2] | pl[3] << 8);
				failed = 1;
			}
		}
	}
	if (!got) {
		fprintf(stderr, "t48-audioctl: no answer\n");
		return 1;
	}
	printf("route=%s mute=%u tone=%s ring=%u ringtone=%s call=%u vol=handset:%u,speaker:%u,headset:%u,ringer:%u,app:%u "
	       "app=%s%s hw=%s%s%s\n",
	       s.route < 4 ? route_names[s.route] : "?", s.mute,
	       s.tone < 6 ? tone_names[s.tone] : "?", s.ring,
	       s.ringtone < T48AC_RINGTONE_COUNT ? ringtone_names[s.ringtone] : "?", s.call,
	       s.vol[1], s.vol[2], s.vol[3], s.vol[4], s.vol_app,
	       s.app < 3 ? app_names[s.app] : "?", s.app_sound ? ",sounding" : "",
	       s.hw & T48AC_HW_CAPTURE ? "capture," : "", s.hw & T48AC_HW_PLAYBACK ? "playback," : "",
	       s.hw & T48AC_HW_RUNNING ? "running" : "idle");
	return failed;
}
