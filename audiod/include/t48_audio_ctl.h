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

/* Volumes the service keeps: one per route, and the ringer */
enum t48ac_vol {
	T48AC_VOL_HANDSET = 1,	/* same numbers as the routes */
	T48AC_VOL_SPEAKER = 2,
	T48AC_VOL_HEADSET = 3,
	T48AC_VOL_RINGER  = 4,
	T48AC_VOL_COUNT
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

enum t48ac_type {
	/* shell to service */
	T48AC_HELLO  = 1,	/* u16 version. Answered with STATE. */
	T48AC_ROUTE  = 2,	/* u8 route */
	T48AC_VOLUME = 3,	/* u8 which (t48ac_vol), u8 level 0..15 */
	T48AC_MUTE   = 4,	/* u8 on: the microphone is silenced towards the call */
	T48AC_TONE   = 5,	/* u8 tone */
	T48AC_RING   = 6,	/* u8 on: an incoming call is alerting */
	T48AC_KEY    = 7,	/* u8 key: '0'-'9', '*', '#' give the DTMF sound,
				 * anything else a click. Feedback for the owner
				 * only; baresip sends the real DTMF. */
	T48AC_GET    = 8,	/* none. Answered with STATE. */

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
	unsigned char vol[T48AC_VOL_COUNT];	/* [0] unused */
	unsigned char hw;		/* bit 0 capture works, bit 1 playback works,
					 * bit 2 the hardware is running now */
	unsigned char pad;
};

#define T48AC_HW_CAPTURE  1
#define T48AC_HW_PLAYBACK 2
#define T48AC_HW_RUNNING  4

/* Where things are heard:
 *  - Call audio: on the route. With route NONE it is dropped.
 *  - TONE and KEY: on the route, at that route's volume. With route NONE
 *    (on-hook dialling) they come from the loudspeaker at the ringer volume.
 *  - RING: with route NONE, the ring melody from the loudspeaker at the
 *    ringer volume. While a route is set (already in a call) it becomes the
 *    call waiting beep on that route, so the melody can never reach an
 *    earpiece. When a ringing call is answered send RING 0 before ROUTE.
 *  - MUTE silences the microphone only. It is cleared by ROUTE NONE.
 */
#endif
