/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The control protocol between the shell (t48-shell) and the audio service
 * (t48-audiod), issue #24. Explained in docs/audiod.md.
 *
 * Transport: Unix stream socket T48AC_SOCKET, owned by the audio service,
 * root only. Framing is the app protocol's (docs/app-protocol.md): u16 type,
 * u16 len, then len bytes; little endian; skip types you do not know.
 *
 * The service keeps no settings of its own. After connecting, the shell
 * sends HELLO and then says how things should be (volumes, route). If the
 * shell goes away the service carries on as it was, so a call survives a
 * shell restart; the shell reads STATE when it comes back.
 */
#ifndef T48_AUDIO_CTL_H
#define T48_AUDIO_CTL_H

#define T48AC_SOCKET   "/run/t48-audiod/ctl.sock"
#define T48AC_VERSION  1
#define T48AC_VOL_MAX  15	/* levels 0 (silent) to 15 (full), 3 dB apart */

/* Where call audio goes. NONE means on hook: the microphone is off and
 * nothing from the call is played. */
enum t48ac_route {
	T48AC_ROUTE_NONE    = 0,
	T48AC_ROUTE_HANDSET = 1,
	T48AC_ROUTE_SPEAKER = 2,
	T48AC_ROUTE_HEADSET = 3,
	T48AC_ROUTE_COUNT
};

/* Volumes the service keeps: one per route, the ringer, and apps' sound */
enum t48ac_vol {
	T48AC_VOL_HANDSET = 1,	/* same numbers as the routes */
	T48AC_VOL_SPEAKER = 2,
	T48AC_VOL_HEADSET = 3,
	T48AC_VOL_RINGER  = 4,
	T48AC_VOL_APP     = 5,
	T48AC_VOL_COUNT
};

/* An app's sound (docs/audiod.md). The shell makes the ring the app writes
 * its samples into (struct t48_sound_ring, t48_proto.h): a memfd of
 * T48AC_APP_BYTES, sealed against changing size, with T48AC_APP_FRAMES
 * samples at T48AC_APP_DATA. It hands the service the descriptor with APP. */
#define T48AC_APP_FRAMES 4096	/* a quarter of a second */
#define T48AC_APP_DATA   4096
#define T48AC_APP_BYTES  (T48AC_APP_DATA + T48AC_APP_FRAMES * 2)

enum t48ac_app {
	T48AC_APP_GONE   = 0,	/* no app has sound: the ring is let go */
	T48AC_APP_PAUSED = 1,	/* off the screen: silent, and what was waiting is dropped */
	T48AC_APP_FRONT  = 2,	/* in front: what it writes is played */
	T48AC_APP_COUNT
};

/* Call-progress tones, made in the service. One at a time; it plays until
 * replaced or stopped with T48AC_TONE_OFF. */
enum t48ac_tone {
	T48AC_TONE_OFF      = 0,
	T48AC_TONE_DIAL     = 1,
	T48AC_TONE_RINGBACK = 2,
	T48AC_TONE_BUSY     = 3,
	T48AC_TONE_ERROR    = 4,	/* number unobtainable, call failed */
	T48AC_TONE_WAITING  = 5,	/* call waiting beep */
	T48AC_TONE_COUNT
};

/* Ringtones (issue #77): the shell picks one per account. CUSTOM is a sound
 * the owner gave the phone: 16 kHz mono s16le samples, at most
 * T48AC_CUSTOM_MAX of them, in a file whose descriptor comes with the RING
 * message (SCM_RIGHTS). Without one the last custom sound is used, or
 * STANDARD if there is none. */
enum t48ac_ringtone {
	T48AC_RINGTONE_STANDARD = 0,	/* two notes in turn, UK double ring */
	T48AC_RINGTONE_LONG     = 1,	/* the same notes, one long ring */
	T48AC_RINGTONE_CHIME    = 2,	/* two falling notes */
	T48AC_RINGTONE_PULSE    = 3,	/* three short beeps */
	T48AC_RINGTONE_CUSTOM   = 4,
	T48AC_RINGTONE_COUNT
};
#define T48AC_CUSTOM_MAX (10 * 16000)	/* ten seconds */

enum t48ac_type {
	/* shell to service */
	T48AC_HELLO  = 1,	/* u16 version. Answered with STATE. */
	T48AC_ROUTE  = 2,	/* u8 route */
	T48AC_VOLUME = 3,	/* u8 which (t48ac_vol), u8 level 0..15 */
	T48AC_MUTE   = 4,	/* u8 on: the microphone is silenced towards the call */
	T48AC_TONE   = 5,	/* u8 tone */
	T48AC_RING   = 6,	/* u8 on: an incoming call is alerting. Then,
				 * optional, u8 t48ac_ringtone (STANDARD if
				 * absent); with CUSTOM, the sound's file as
				 * a descriptor beside the message */
	T48AC_KEY    = 7,	/* u8 key: '0'-'9', '*', '#' give the DTMF sound,
				 * anything else a click. Feedback for the owner
				 * only; baresip sends the real DTMF. */
	T48AC_GET    = 8,	/* none. Answered with STATE. */
	T48AC_APP    = 9,	/* u8 t48ac_app. A descriptor sent with it
				 * (SCM_RIGHTS) is the ring of a new app and
				 * replaces any other. */

	/* service to shell */
	T48AC_STATE  = 0x81,	/* struct t48ac_state; sent in answer to HELLO
				 * and GET, and to every connected client
				 * whenever anything in it changes */
	T48AC_ERROR  = 0x82,	/* u16 type of the message refused, u16 reason */
};

enum t48ac_error {
	T48AC_ERR_VERSION = 1,
	T48AC_ERR_LENGTH  = 2,
	T48AC_ERR_VALUE   = 3,
	T48AC_ERR_RING    = 4,	/* APP: no ring, or not one made as above */
};

/* Payload of STATE. All single bytes, so no byte order to mind. Later
 * versions may add bytes at the end. */
struct t48ac_state {
	unsigned char version;
	unsigned char route;		/* t48ac_route */
	unsigned char mute;
	unsigned char tone;		/* t48ac_tone */
	unsigned char ring;
	unsigned char call;		/* 1 while baresip's call stream is connected */
	unsigned char vol[5];		/* [0] unused, then handset, speaker,
					 * headset, ringer */
	unsigned char hw;		/* bit 0 capture works, bit 1 playback works,
					 * bit 2 the hardware is running now */
	unsigned char vol_app;		/* T48AC_VOL_APP */
	unsigned char app;		/* t48ac_app, as last told */
	unsigned char app_sound;	/* 1 while an app's sound is being played */
	unsigned char ringtone;		/* t48ac_ringtone the ringer plays (CUSTOM
					 * only once a sound is loaded) */
};

#define T48AC_HW_CAPTURE  1
#define T48AC_HW_PLAYBACK 2
#define T48AC_HW_RUNNING  4

/* Where things are heard:
 *  - Call audio: on the route. With route NONE it is dropped.
 *  - TONE and KEY: on the route, at that route's volume. With route NONE
 *    (on-hook dialling) they come from the loudspeaker at the ringer volume.
 *  - RING: with route NONE, the ring melody (its ringtone) from the
 *    loudspeaker at the ringer volume. While a route is set (already in a call) it becomes the
 *    call waiting beep on that route, so the melody can never reach an
 *    earpiece. When a ringing call is answered send RING 0 before ROUTE.
 *  - MUTE silences the microphone only. It is cleared by ROUTE NONE.
 *  - An app's sound: from the loudspeaker at the app volume, through the
 *    loudspeaker's limit. With a route set and nothing else on it, on that
 *    route. Calls win: while RING is on, a TONE plays, or a route is set
 *    with the call stream up, it is taken at the same rate and not played.
 */
#endif
