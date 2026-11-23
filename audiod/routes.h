/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48-audiod: the route table (issue #24). A route says which codec to
 * record from, which to play to, what echo control to use, and the mixer
 * controls to set. The compiled-in table follows the daytime tests on the
 * phone (docs/audio.md); a routes file can replace any of it without a
 * rebuild.
 */
#ifndef AUDIOD_ROUTES_H
#define AUDIOD_ROUTES_H
#include "echo.h"
#include "include/t48_audio_ctl.h"

#define ROUTE_MAX_CTL 24

/* Rows: the three call routes by their t48ac_route number, and ALERT, the
 * loudspeaker for ringing and on-hook tones (used while the route is NONE) */
#define ROUTE_ALERT 0
#define ROUTE_ROWS  T48AC_ROUTE_COUNT

struct route_ctl {
	char name[44];		/* ALSA mixer control name */
	int value;
};

struct route {
	char name[12];
	int cap_dev;		/* ALSA device to record from: 0 codec A, 1 codec B */
	int play_dev;		/* ALSA device to play to */
	enum echo_mode echo;
	int coupling_q8;	/* see echo.h */
	int confirmed;		/* 0 until someone has checked it on a phone */
	int nctl;
	struct route_ctl ctl[ROUTE_MAX_CTL];
};

extern struct route routes[ROUTE_ROWS];

/* Read a routes file over the table. Returns 0, or -1 after printing why. */
int routes_load(const char *path);
void routes_print(void);

#endif
