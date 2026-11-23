// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: the route table. See routes.h and docs/audiod.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "routes.h"

/*
 * NOT CONFIRMED ON A PHONE. This is "Reading 1" of docs/audio.md: handset
 * microphone on codec A input A, headset on codec A input B, base microphone
 * on codec B input B, loudspeaker on codec B output B. Capture control names
 * are the driver's. Playback control names are the ones the driver is asked
 * to provide (docs/audiod.md); a control the kernel does not have is reported
 * once and skipped. Gains are Yealink's idle values where known.
 */
#define CAP_A(on, src) \
	{ "Codec A Capture Switch", on }, { "Codec A Capture Source", src }, \
	{ "Codec A Preamp Capture Volume", 5 }, { "Codec A Capture Volume", 36 }
#define CAP_B(on) \
	{ "Codec B Capture Switch", on }, { "Codec B Capture Source", 1 }, \
	{ "Codec B Preamp Capture Volume", 5 }, { "Codec B Capture Volume", 36 }
#define OUT(aa, ab, ba, bb, amp) \
	{ "Codec A Output A Playback Switch", aa }, { "Codec A Output B Playback Switch", ab }, \
	{ "Codec B Output A Playback Switch", ba }, { "Codec B Output B Playback Switch", bb }, \
	{ "Speaker Amp Switch", amp }
#define SIDETONE(a, b) \
	{ "Codec A Sidetone Switch", a }, { "Codec B Sidetone Switch", b }

struct route routes[ROUTE_ROWS] = {
	[ROUTE_ALERT] = {
		.name = "alert", .cap_dev = 1, .play_dev = 1, .echo = ECHO_NONE,
		.nctl = 15, .ctl = { CAP_A(0, 0), CAP_B(0), SIDETONE(0, 0), OUT(0, 0, 0, 1, 1) },
	},
	[T48AC_ROUTE_HANDSET] = {
		.name = "handset", .cap_dev = 0, .play_dev = 0, .echo = ECHO_NONE,
		.nctl = 15, .ctl = { CAP_A(1, 0), CAP_B(0), OUT(1, 0, 0, 0, 0), SIDETONE(1, 0) },
	},
	[T48AC_ROUTE_SPEAKER] = {
		.name = "speaker", .cap_dev = 1, .play_dev = 1, .echo = ECHO_SWITCH,
		.coupling_q8 = 256,
		.nctl = 15, .ctl = { CAP_A(0, 0), CAP_B(1), SIDETONE(0, 0), OUT(0, 0, 0, 1, 1) },
	},
	[T48AC_ROUTE_HEADSET] = {
		.name = "headset", .cap_dev = 0, .play_dev = 0, .echo = ECHO_NONE,
		.nctl = 15, .ctl = { CAP_A(1, 1), CAP_B(0), OUT(0, 1, 0, 0, 0), SIDETONE(1, 0) },
	},
};

static char *trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		*--e = 0;
	return s;
}

/*
 * The file:
 *   [handset]              one of: alert handset speaker headset
 *   capture 0              ALSA device to record from
 *   playback 0             ALSA device to play to
 *   echo none              or: switch
 *   coupling 256           echo level against loudspeaker level, 256 = equal
 *   confirmed 1
 *   set Codec A Capture Source = 0
 * The first "set" in a section clears that route's compiled-in list.
 */
int routes_load(const char *path)
{
	FILE *f = fopen(path, "r");
	struct route *r = 0;
	char line[160];
	int ln = 0, cleared = 0;

	if (!f) {
		perror(path);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line), *v;
		int i;

		ln++;
		if (!*s || *s == '#')
			continue;
		if (*s == '[') {
			char *e = strchr(s, ']');
			if (e)
				*e = 0;
			for (r = 0, i = 0; i < ROUTE_ROWS; i++)
				if (!strcmp(routes[i].name, s + 1))
					r = &routes[i];
			cleared = 0;
			if (!r)
				goto bad;
			continue;
		}
		if (!r)
			goto bad;
		if (!strncmp(s, "set ", 4)) {
			v = strrchr(s, '=');
			if (!v)
				goto bad;
			*v++ = 0;
			s = trim(s + 4);
			if (!cleared) {
				r->nctl = 0;
				cleared = 1;
			}
			if (r->nctl == ROUTE_MAX_CTL || strlen(s) >= sizeof(r->ctl[0].name))
				goto bad;
			strcpy(r->ctl[r->nctl].name, s);
			r->ctl[r->nctl++].value = atoi(v);
			continue;
		}
		v = strpbrk(s, " \t");
		if (!v)
			goto bad;
		*v++ = 0;
		v = trim(v);
		if (!strcmp(s, "capture"))
			r->cap_dev = atoi(v);
		else if (!strcmp(s, "playback"))
			r->play_dev = atoi(v);
		else if (!strcmp(s, "coupling"))
			r->coupling_q8 = atoi(v);
		else if (!strcmp(s, "confirmed"))
			r->confirmed = atoi(v);
		else if (!strcmp(s, "echo") && !strcmp(v, "none"))
			r->echo = ECHO_NONE;
		else if (!strcmp(s, "echo") && !strcmp(v, "switch"))
			r->echo = ECHO_SWITCH;
		else
			goto bad;
	}
	fclose(f);
	return 0;
bad:
	fprintf(stderr, "t48-audiod: %s line %d: not understood\n", path, ln);
	fclose(f);
	return -1;
}

void routes_print(void)
{
	int i, j;
	for (i = 0; i < ROUTE_ROWS; i++) {
		const struct route *r = &routes[i];
		printf("[%s]\ncapture %d\nplayback %d\necho %s\ncoupling %d\nconfirmed %d\n",
		       r->name, r->cap_dev, r->play_dev,
		       r->echo == ECHO_SWITCH ? "switch" : "none", r->coupling_q8, r->confirmed);
		for (j = 0; j < r->nctl; j++)
			printf("set %s = %d\n", r->ctl[j].name, r->ctl[j].value);
		printf("\n");
	}
}
