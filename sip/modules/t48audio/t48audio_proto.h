/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The call stream between baresip (module t48audio) and the audio service
 * t48-audiod. PROPOSED in issue #25; issue #24 owns the service and may
 * change this before anything speaks it. Explained in docs/sip.md.
 *
 * Transport: Unix stream socket T48A_SOCKET, owned by the audio service,
 * mode 0600 for the user baresip runs as. Framing as the app protocol:
 * u16 type, u16 len, then len bytes; little endian; skip unknown types.
 *
 *   baresip -> service  T48A_HELLO  u16 version, u16 ptime_ms
 *   service -> baresip  T48A_READY  u16 version
 *   service -> baresip  T48A_MIC    ptime worth of samples (s16le, 16 kHz, mono)
 *   baresip -> service  T48A_SPK    the same amount, sent in answer to each MIC
 *   baresip -> service  T48A_BYE    none; or just close the socket
 *
 * The service is the clock: it sends MIC every ptime from the codec's DMA
 * interrupt, and plays each SPK as it arrives (a small queue on its side
 * absorbs scheduling jitter). A missing SPK is played as silence.
 *
 * Not on this socket: routes (handset, headset, speaker), volume, mute,
 * ringing and tones. The shell asks the service for those on its own
 * control socket, so a baresip restart cannot change them.
 */
#ifndef T48AUDIO_PROTO_H
#define T48AUDIO_PROTO_H

#define T48A_SOCKET     "/run/t48-audiod/call.sock"
#define T48A_VERSION    1
#define T48A_SRATE      16000
#define T48A_MAX_PTIME  40

enum t48a_type {
	T48A_HELLO = 1,
	T48A_READY = 2,
	T48A_MIC   = 3,
	T48A_SPK   = 4,
	T48A_BYE   = 5,
};

#endif
