# SIP: baresip on the phone

Issue [#25](https://github.com/M0LTE/yealink-t48s/issues/25). Design: [architecture.md](architecture.md#sip-stack). Files in [`sip/`](../sip).

## Summary

- baresip 4.13.0 and libre 4.13.0 (both BSD-3-Clause, current releases) build as one static ARMv6 musl soft-float binary of **956 KB**, with only the modules we need. With SIP over TLS it is **3.3 MB**, because TLS in libre means OpenSSL.
- Proved off the phone, through a throwaway Kamailio registrar on loopback: two instances **register with a password** (one over UDP, one over TCP; a wrong password is refused), one **dials**, the other **answers**, both **hear the other's tone**, in G.711 and in G.722. The **ARM binary does the same under qemu-arm**: registers, calls, sends and receives audio.
- Calls are driven over baresip's **ctrl_tcp** interface (JSON in netstrings: commands in, answers and events out). That is the protocol the shell should speak. It listens on loopback for now; our own module will serve the same protocol on a root-only Unix socket.
- Our own audio module, **t48audio**, is in the binary as an interface stub: the boundary with the audio service (#24) is written down in [`t48audio_proto.h`](../sip/modules/t48audio/t48audio_proto.h). Until the service exists it is a null device, and it carried a real call in the test.

## What is built

| Module | Why |
|---|---|
| `account` | reads the accounts file that `t48-sip` writes |
| `menu` | the call commands (`dial`, `accept`, `hangup`, `reginfo`, ...) that ctrl_tcp runs |
| `g711`, `libg722` | codecs: G.711 (PCMU, PCMA) and G.722. G.722 is not inside baresip: `libg722` wraps sippy's [libg722](https://github.com/sippy/libg722) (two C files, BSD-2-Clause and public domain) |
| `auresamp` | converts the 16 kHz device audio to and from 8 kHz for G.711 |
| `t48audio` | ours: the call stream to the audio service (stub, see below) |
| `aufile`, `ausine` | WAV file and tone audio, for tests |
| `ctrl_tcp` | JSON control for the shell |
| `stdio` | a console for trying things by hand |

libre is built without BFCP, PCP, RTMP, zlib and backtraces. It needs a crypto library even without TLS (MD5 for digest login, SHA-1 and HMAC for STUN), so the default build links **mbedTLS 3.6.8** for those alone. libre only does TLS through **OpenSSL**, so `TLS=1` swaps mbedTLS for OpenSSL 3.5.9 (LTS) with the unused parts configured out.

| Binary (stripped) | Size |
|---|---|
| ARM, no TLS (the default) | 979,376 bytes |
| ARM, with TLS | 3,464,920 bytes |
| x86-64, for the PC test | 1,058,424 bytes |

TLS costs about 2.4 MB of flash and RAM. Plain UDP and TCP first, as the design says; TLS is one build switch away when wanted.

One patch to libre ([`sip/patches`](../sip/patches)): it reads an address flag (`IFA_FLAGS`) that arrived in Linux 3.14, and we build against our 3.6.11 kernel's headers, whose kernel never sends it.

## Building

```
docker build -t t48-sip sip
docker run --rm -u $(id -u):$(id -g) -e HOME=/work -e T=/work/t48s \
    -v "$PWD":/src -v "$HOME/t48-work":/work -w /src t48-sip sh -c \
    'native/build-toolchain.sh && sip/build.sh && TARGET=host sip/build.sh && sip/test/call-test.sh'
```

- `native/build-toolchain.sh` makes the musl cross toolchain (`$T/bin/armcc`) that `native/Makefile` already expects, now with the 3.6.11 kernel headers in its sysroot.
- `sip/build.sh` downloads pinned, checksummed sources and builds into `$T/sip-arm/bin/baresip`. `TARGET=host` builds the same thing for the PC; `TLS=1` the TLS variant. Licence texts of everything linked in are copied to `$T/sip-*/share/licences/`; they ship with the binary.
- A build takes a minute or two on 8 cores. The container image is about 2 GB.

These are plain scripts because Buildroot (#16) had not landed when this was written. They map directly onto Buildroot packages: one each for libre (cmake-package, depends on mbedtls or openssl, which Buildroot already has), libg722 and baresip (with `APP_MODULES_DIR` pointing at `sip/modules`), plus the libre patch.

## Running it

`t48-sip` is the program runit starts. It reads the owner's account from `/data/config/sip`, writes baresip's accounts file into `/run/t48-sip` (mode 0700, as it holds the password) beside a copy of the config, and runs baresip there.

```
# /data/config/sip  (see sip/config/sip.example)
user=1001
password=...
domain=sip.example.com
# optional: auth_user, display_name, transport (udp, tcp, tls), outbound_proxy, register_seconds
```

The config is [`sip/config/config`](../sip/config/config), installed as `/usr/share/t48/sip/config`; `/data/local/sip/config` replaces it. No credentials are in the repository; the test's passwords are made up for the throwaway registrar.

## Control interface

| Interface | Verdict |
|---|---|
| `stdio` | one terminal, text for people. For hand testing only. |
| `cons` | text console over UDP/TCP: commands work, but the output is text for people and events are not structured. |
| **`ctrl_tcp`** | **chosen.** JSON commands with a token, a JSON answer for each, and JSON events (`REGISTER_OK`, `CALL_INCOMING`, `CALL_ESTABLISHED`, `CALL_CLOSED`, ...) with the account, peer and call id. Netstring framing (`<length>:<json>,`), trivial to parse in C. |
| `ctrl_dbus`, `mqtt` | ruled out: need D-Bus or a broker. |

```
-> 61:{"command":"dial","params":"sip:bob@example.com","token":"1"},
<- {"response":true,"ok":true,"data":"","token":"1"}
<- {"event":true,"class":"call","type":"CALL_ESTABLISHED","accountaor":"sip:alice@...","direction":"outgoing","peeruri":"sip:bob@...","id":"..."}
```

The commands the shell needs: `dial <uri>`, `accept`, `hangup`, `hold`, `resume`, `mute`, `transfer <uri>`, `sndcode <digit>` (DTMF, sent as RFC 4733 events by default), `reginfo`, `listcalls`.

The catch: ctrl_tcp only listens on TCP, so any local program, including an app running as its own user, could connect and place calls. For now it listens on `127.0.0.1:4444` only, which is fine while we write our own apps. The design asks for a Unix socket only root can open: that is a small module of our own (`t48ctrl`, not written yet) serving **the same protocol**, adapted from ctrl_tcp's 400 lines. The shell's client code then changes only in how it connects.

## Audio: the boundary with the audio service

`t48audio` ([source](../sip/modules/t48audio/t48audio.c)) is baresip's audio source and player for the phone. The proposed contract ([`t48audio_proto.h`](../sip/modules/t48audio/t48audio_proto.h)), for #24 to accept or change:

- **One call stream** per baresip process, on a Unix socket the audio service owns (`/run/t48-audiod/call.sock`). Framing as the app protocol: `u16 type, u16 len`, payload, little endian.
- **Fixed format: 16 kHz, mono, signed 16-bit**, what the codecs' DMA runs at. baresip resamples for G.711 (`ausrc_srate`/`auplay_srate 16000` in the config).
- **The audio service is the clock.** Every ptime (20 ms) it sends one MIC frame; baresip answers each with one SPK frame. Both directions run off the codec's own clock, so nothing drifts, and a missing SPK frame is simply silence.
- **Not on this socket:** routes (handset, headset, speaker), volume, mute, ringing and tones. The shell asks the audio service for those directly, so they survive a baresip restart and baresip never needs to know which path is live.

Today the stub does not connect to anything: a thread stands in for the service's clock, feeds silence as the microphone and drops the speaker audio. That makes it the null backend the issue asked for; the test ran a full G.722 call through it. When the service exists only two functions change (`stream_open`, `stream_tick`).

baresip's own `alsa` module is not built. In the design only the audio service touches the hardware; `alsa` would also pull in alsa-lib. It could still be useful for one quick on-phone test before #24 exists, at a size cost not yet measured.

## The test

`sip/test/call-test.sh` (in the build container) starts Kamailio on loopback with digest login for REGISTER and INVITE ([`kamailio.cfg`](../sip/test/kamailio.cfg)), then runs alice and bob through `t48-sip` with settings files, as on the phone. [`ctrl.py`](../sip/test/ctrl.py) checks both are registered, makes alice dial and bob answer over ctrl_tcp, waits 4 s, hangs up, and checks each recording for the other side's tone (alice sends 440 Hz, bob 1000 Hz).

Result on 2026-11-23, all passing:

| Check | Result |
|---|---|
| Registration with password: alice over UDP, bob over TCP | both `OK`, the registrar saved both |
| Wrong password | `401 Unauthorized`, refused |
| G.711 (PCMU) call, dialled and answered over ctrl_tcp | bob heard 440 Hz, alice heard 1000 Hz |
| G.722 call | the same |
| G.722 call with bob on `t48audio` | established and closed cleanly; alice heard silence, as she should |
| G.722 call with **alice as the ARM binary under qemu-arm** | bob heard 440 Hz, alice heard 1000 Hz |

## What's left for the phone

1. Copy the ARM binary, `t48-sip`, the config and a settings file to the phone under our kernel and register to a real SIP server; dial out and in from an SSH session with `nc`/a small client on ctrl_tcp. The null backend is enough for this.
2. Measure CPU for a G.722 call on the 500 MHz ARM11 (expected small; G.711 is trivial).
3. Our control module on a Unix socket (`t48ctrl`), and the shell's client for it (#26).
4. The real `t48audio`, once the audio service (#24) agrees the contract above. Echo cancelling for the speakerphone lives in the service.
5. A runit service for `t48-sip`; Buildroot packages once #16 lands.
6. Decide on TLS (2.4 MB) and SRTP later.

## Licences

baresip and libre are BSD-3-Clause, libg722 BSD-2-Clause and public domain, mbedTLS and OpenSSL Apache-2.0. Their sources are used unmodified apart from the one patch above, and their licence texts are installed with the binary. Our files in `sip/` are AGPL-3.0-or-later.
