// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: the route table. See routes.h and docs/audiod.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "routes.h"

/*
 * From the daytime tests on the phone (docs/audio.md, 2026-11-23): codec B is
 * the handset (input B, output B) and the headset (input A, output A); codec A
 * with the external amplifier is the speakerphone (input A, output A). Control
 * names are the driver's (kernel/drivers/t48s_audio.c); a control the kernel
 * does not have is reported once and skipped.
 *
 * The values are the ones Yealink's firmware was seen to set in each mode:
 *   handset  preamp 15 dB, ADC 0 dB, DAC +3 dB, output B at -9 dB, sidetone on
 *   headset  preamp 22 dB (21 here: the control moves in 3 dB steps), ADC 0 dB,
 *            DAC +3 dB, output A at -6 dB, sidetone on
 *   speaker  preamp 21 dB, ADC +1 dB, DAC -6 dB, output A at -3 dB, amplifier on
 * with one change: the speakerphone's ADC gain is +12 dB, not +1, because the
 * base microphone is quiet at arm's length (daytime test 2) and Yealink makes
 * that up in its DSP.
 *
 * Order matters for silence: amplifier and the other outputs off first, then
 * gains, then this route's output on, and the amplifier last.
 */
#define MIC_A(on, digital) \
	{ "Codec A Capture Switch", on }, { "Codec A Capture Source", 0 }, \
	{ "Codec A Preamp Capture Volume", 7 }, { "Codec A Capture Volume", digital }
#define MIC_B(on, src, preamp) \
	{ "Codec B Capture Switch", on }, { "Codec B Capture Source", src }, \
	{ "Codec B Preamp Capture Volume", preamp }, { "Codec B Capture Volume", 36 }
#define QUIET_A \
	{ "Speaker Amp Switch", 0 }, { "Codec A Output A Playback Switch", 0 }, \
	{ "Codec A Output B Playback Switch", 0 }, { "Codec A Sidetone Switch", 0 }
#define QUIET_B \
	{ "Codec B Output A Playback Switch", 0 }, { "Codec B Output B Playback Switch", 0 }, \
	{ "Codec B Sidetone Switch", 0 }
#define SPEAKER_OUT \
	{ "Codec A Output B Playback Switch", 0 }, { "Codec A Sidetone Switch", 0 }, \
	{ "Codec A Playback Volume", 30 }, { "Codec A Output A Playback Volume", 6 }, \
	{ "Codec A Output A Playback Switch", 1 }, { "Speaker Amp Switch", 1 }

struct route routes[ROUTE_ROWS] = {
	[ROUTE_ALERT] = {
		.name = "alert", .cap_dev = 0, .play_dev = 0, .echo = ECHO_NONE, .confirmed = 1,
		.nctl = 17, .ctl = { QUIET_B, MIC_B(0, 1, 5), MIC_A(0, 48), SPEAKER_OUT },
	},
	[T48AC_ROUTE_HANDSET] = {
		.name = "handset", .cap_dev = 1, .play_dev = 1, .echo = ECHO_NONE, .confirmed = 1,
		.nctl = 18, .ctl = { QUIET_A, { "Codec B Output A Playback Switch", 0 },
			MIC_A(0, 48), MIC_B(1, 1, 5),
			{ "Codec B Playback Volume", 39 }, { "Codec B Output B Playback Volume", 4 },
			{ "Codec B Sidetone Volume", 18 }, { "Codec B Sidetone Switch", 1 },
			{ "Codec B Output B Playback Switch", 1 } },
	},
	[T48AC_ROUTE_SPEAKER] = {
		.name = "speaker", .cap_dev = 0, .play_dev = 0, .echo = ECHO_SWITCH, .confirmed = 1,
		.coupling_q8 = 512,	/* measured: the base microphone hears a tone up to 7 dB above its level in the frame played (1.4 kHz) */
		.nctl = 17, .ctl = { QUIET_B, MIC_B(0, 1, 5), MIC_A(1, 48), SPEAKER_OUT },
	},
	[T48AC_ROUTE_HEADSET] = {
		/* settings as Yealink's; not tried with a headset plugged in */
		.name = "headset", .cap_dev = 1, .play_dev = 1, .echo = ECHO_NONE,
		.nctl = 18, .ctl = { QUIET_A, { "Codec B Output B Playback Switch", 0 },
			MIC_A(0, 48), MIC_B(1, 0, 7),
			{ "Codec B Playback Volume", 39 }, { "Codec B Output A Playback Volume", 5 },
			{ "Codec B Sidetone Volume", 18 }, { "Codec B Sidetone Switch", 1 },
			{ "Codec B Output A Playback Switch", 1 } },
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
