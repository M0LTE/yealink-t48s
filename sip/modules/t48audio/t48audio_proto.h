/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The call stream between baresip (module t48audio) and the audio service
 * t48-audiod. Proposed in issue #25, agreed with changes in issue #24.
 * Explained in docs/audiod.md.
 *
 * Transport: Unix stream socket T48A_SOCKET, owned by the audio service,
 * mode 0600 for the user baresip runs as. Framing as the app protocol:
 * u16 type, u16 len, then len bytes; little endian; skip unknown types.
 *
 *   baresip -> service  T48A_HELLO  u16 version, u16 frame_ms (20)
 *   service -> baresip  T48A_READY  u16 version, u16 frame_ms (20)
 *   service -> baresip  T48A_MIC    u32 seq, then T48A_FRAME samples
 *   baresip -> service  T48A_SPK    u32 seq of the MIC it answers, then
 *                                   T48A_FRAME samples
 *   baresip -> service  T48A_BYE    none; or just close the socket
 *
 * Samples are signed 16-bit little endian, 16 kHz, mono. A frame is always
 * 20 ms (320 samples) whatever packet time the codec uses: baresip re-cuts
 * the audio for the codec itself.
 *
 * The service is the clock: it sends MIC every 20 ms from the codec's own
 * sample clock, and baresip answers each MIC at once with one SPK. The
 * service plays one SPK per tick; a missing one is played as silence. seq
 * counts MIC frames from 0 for each connection.
 *
 * The service closes the connection on a HELLO it cannot serve. Only one
 * call stream exists: a new connection replaces the old one.
 *
 * Not on this socket: routes (handset, headset, speaker), volume, mute,
 * ringing and tones. The shell asks the service for those on its own
 * control socket (audiod/include/t48_audio_ctl.h), so a baresip restart
 * cannot change them. With no route set the service sends silence as MIC
 * and does not play SPK.
 */
#ifndef T48AUDIO_PROTO_H
#define T48AUDIO_PROTO_H

#define T48A_SOCKET     "/run/t48-audiod/call.sock"
#define T48A_VERSION    2
#define T48A_SRATE      16000
#define T48A_FRAME_MS   20
#define T48A_FRAME      (T48A_SRATE * T48A_FRAME_MS / 1000)	/* samples */
#define T48A_FRAME_LEN  (4 + T48A_FRAME * 2)			/* MIC, SPK payload */

enum t48a_type {
	T48A_HELLO = 1,
	T48A_READY = 2,
	T48A_MIC   = 3,
	T48A_SPK   = 4,
	T48A_BYE   = 5,
};

#endif
