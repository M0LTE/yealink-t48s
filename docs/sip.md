# SIP: baresip on the phone

Issue [#25](https://github.com/M0LTE/yealink-t48s/issues/25). Design: [architecture.md](architecture.md#sip-stack). Files in [`sip/`](../sip).

## Summary

- baresip 4.13.0 and libre 4.13.0 (both BSD-3-Clause, current releases) build as one static ARMv6 musl soft-float binary of **956 KB**, with only the modules we need. With SIP over TLS it is **3.3 MB**, because TLS in libre means OpenSSL.
- Proved off the phone, through a throwaway Kamailio registrar on loopback: two instances **register with a password** (one over UDP, one over TCP; a wrong password is refused), one **dials**, the other **answers**, both **hear the other's tone**, in G.711 and in G.722. The **ARM binary does the same under qemu-arm**: registers, calls, sends and receives audio.
- Calls are driven with JSON in netstrings: commands in, answers and events out. That is the protocol of baresip's ctrl_tcp module, served by our own module **t48ctrl** on a Unix socket only root can open, so apps cannot place calls. The shell speaks it ([phone.md](phone.md)).
- Our own audio module, **t48audio**, carries the call's audio to and from the audio service `t48-audiod` (#24, [audiod.md](audiod.md)). The boundary is written down in [`t48audio_proto.h`](../sip/modules/t48audio/t48audio_proto.h). With no audio service running it is a null device, so SIP work does not depend on it.

## What is built

| Module | Why |
|---|---|
| `account` | reads the accounts file that `t48-sip` writes |
| `menu` | the call commands (`dial`, `accept`, `hangup`, `reginfo`, ...) that t48ctrl runs |
| `t48mwi` | ours: asks each account's server about waiting voicemail (RFC 3842) and passes the answer on as an event, for the message lamp. baresip's own `mwi` module is not built: it can spin for ever at start when an account registers before its start-up timer runs (seen against a server on the same machine), and it delays shutdown by half a second |
| `g711`, `libg722` | codecs: G.711 (PCMU, PCMA) and G.722. G.722 is not inside baresip: `libg722` wraps sippy's [libg722](https://github.com/sippy/libg722) (two C files, BSD-2-Clause and public domain) |
| `auresamp` | converts the 16 kHz device audio to and from 8 kHz for G.711 |
| `t48audio` | ours: the call stream to the audio service (see below) |
| `aufile`, `ausine` | WAV file and tone audio, for tests |
| `t48ctrl` | ours: JSON control for the shell, on a root-only Unix socket (see below) |
| `stdio` | a console for trying things by hand |

libre is built without BFCP, PCP, RTMP, zlib and backtraces. It needs a crypto library even without TLS (MD5 for digest login, SHA-1 and HMAC for STUN), so the default build links **mbedTLS 3.6.8** for those alone. libre only does TLS through **OpenSSL**, so `TLS=1` swaps mbedTLS for OpenSSL 3.5.9 (LTS) with the unused parts configured out.

| Binary (stripped) | Size |
|---|---|
| ARM, no TLS (the default) | 982,968 bytes |
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

The phone's image builds the same thing with Buildroot (#16): packages `libre`, `libg722` and `baresip` in `buildroot/package`, with the same versions and checksums, the libre patch from `sip/patches`, Buildroot's mbedTLS, and the module list read from `sip/build.sh`. They install `baresip`, `t48-sip`, the config and `sip.example` in `/usr/share/t48/sip`, and the licence texts; nothing starts baresip at boot. The scripts stay for the PC build and the call test.

## Running it

`t48-sip` is the program runit starts. It reads the owner's accounts from `/data/config/sip` (account 1) and `sip2` to `sip6` beside it (#36), leaves out any that are switched off or have no user or domain, writes baresip's accounts file into `/run/t48-sip` (mode 0700, as it holds the password) beside a copy of the config, and runs baresip there.

```
# /data/config/sip  (see sip/config/sip.example)
user=1001
password=...
domain=sip.example.com
# optional: auth_user, display_name, transport (udp, tcp, tls), outbound_proxy, register_seconds,
#           enabled (off), mwi (off); label and voicemail are for the shell
```

When an account is changed in Settings, `t48-apply sip` kills baresip and starts `t48-sip` again a second later. `t48-sip` keeps its process number in the run directory and waits (up to five seconds) for an old baresip to let go of the SIP port before starting the new one.

The config is [`sip/config/config`](../sip/config/config), installed as `/usr/share/t48/sip/config`; `/data/local/sip/config` replaces it. No credentials are in the repository; the test's passwords are made up for the throwaway registrar.

## Control interface

| Interface | Verdict |
|---|---|
| `stdio` | one terminal, text for people. For hand testing only. |
| `cons` | text console over UDP/TCP: commands work, but the output is text for people and events are not structured. |
| **`ctrl_tcp`** | **its protocol chosen**, but not the module itself (see the catch below). JSON commands with a token, a JSON answer for each, and JSON events (`REGISTER_OK`, `CALL_INCOMING`, `CALL_ESTABLISHED`, `CALL_CLOSED`, ...) with the account, peer and call id. Netstring framing (`<length>:<json>,`), trivial to parse in C. |
| `ctrl_dbus`, `mqtt` | ruled out: need D-Bus or a broker. |

```
-> 61:{"command":"dial","params":"sip:bob@example.com","token":"1"},
<- {"response":true,"ok":true,"data":"","token":"1"}
<- {"event":true,"class":"call","type":"CALL_ESTABLISHED","accountaor":"sip:alice@...","direction":"outgoing","peeruri":"sip:bob@...","id":"..."}
```

The commands the shell needs: `dial <uri>`, `accept`, `hangup`, `hold`, `resume`, `mute`, `transfer <uri>`, `sndcode <digit>` (DTMF, sent as RFC 4733 events by default), `reginfo`, `listcalls`. Those that act on a call take its id, which is how the shell handles two calls.

The catch: ctrl_tcp only listens on TCP, so any local program, including an app running as its own user, could connect and place calls. So it is not built. Our own module, **t48ctrl** ([source](../sip/modules/t48ctrl/t48ctrl.c)), serves the same protocol on a Unix socket:

- **Where:** `ctrl.sock` in `t48-sip`'s run directory, so `/run/t48-sip/ctrl.sock` on the phone (`t48ctrl_socket` in the config moves it).
- **Who:** the socket is mode 0600 inside a 0700 directory, and the module also checks each connection: only root, or the user baresip runs as, gets in.
- **Several clients at once** (the shell, and a tool over SSH); each gets every event.
- **Catching up:** a client that has just connected is told how things stand, as events marked `"sync":true`: one `REGISTER_OK`, `REGISTER_FAIL` or `REGISTERING` for each account and one `CALL_INCOMING`, `CALL_OUTGOING`, `CALL_RINGING` or `CALL_ESTABLISHED` for each call (with `onhold` and `duration`). That is how a restarted shell picks a call back up. The command `sync` asks for the same again.
- Level meter and RTCP report events are left out; the shell has no use for them.
- **Which account:** a command may carry `"account":"sip:user@domain"` and is then run for that account, so `dial` calls out from it. Every event already says which account it is about (`accountaor`).
- **`joincalls <call-id> <call-id>`** is ours: it connects the two people we are talking to with each other and takes us out (an attended transfer, REFER with Replaces). The first call is the one sent away, the second where it is sent.
- `MWI_NOTIFY` events (from `t48mwi`) carry the server's message-summary text in `param`.

To try it by hand: `python3 sip/test/far.py /run/t48-sip/ctrl.sock cmd reginfo`.

## Audio: the boundary with the audio service

`t48audio` ([source](../sip/modules/t48audio/t48audio.c)) is baresip's audio source and player for the phone. The contract ([`t48audio_proto.h`](../sip/modules/t48audio/t48audio_proto.h)) was proposed here and agreed in #24 with small changes; [audiod.md](audiod.md#the-call-stream-baresip) has the details.

- **One call stream** per baresip process, shared by all its calls: the phone can have two (one on hold, or one being asked before a transfer). Each call's source gets the microphone frame and the players' frames are added together; a held call sends and plays nothing. The stream is on a Unix socket the audio service owns (`/run/t48-audiod/call.sock`; `t48audio_socket` in the config moves it). Framing as the app protocol: `u16 type, u16 len`, payload, little endian.
- **Fixed format: 16 kHz, mono, signed 16-bit, always 20 ms frames.** baresip resamples for G.711 (`ausrc_srate`/`auplay_srate 16000` in the config) and re-cuts frames for the codec's packet time.
- **The audio service is the clock.** Every 20 ms it sends one MIC frame; baresip answers each with one SPK frame. Both directions run off the codec's own clock, so nothing drifts, and a missing SPK frame is simply silence.
- **Not on this socket:** routes (handset, headset, speaker), volume, mute, ringing and tones. The shell asks the audio service for those directly, so they survive a baresip restart and baresip never needs to know which path is live.
- **No audio service, no problem:** the module then keeps its own clock, feeds silence as the microphone, drops the speaker audio and retries the socket twice a second. A call survives the audio service restarting.

baresip's own `alsa` module is not built. In the design only the audio service touches the hardware; `alsa` would also pull in alsa-lib. It could still be useful for one quick on-phone test before #24 exists, at a size cost not yet measured.

## The test

`sip/test/call-test.sh` (in the build container) starts Kamailio on loopback with digest login for REGISTER and INVITE ([`kamailio.cfg`](../sip/test/kamailio.cfg)), then runs alice and bob through `t48-sip` with settings files, as on the phone. [`ctrl.py`](../sip/test/ctrl.py) checks both are registered, makes alice dial and bob answer over t48ctrl, waits 4 s, hangs up, and checks each recording for the other side's tone (alice sends 440 Hz, bob 1000 Hz).

Result on 2026-11-23, all passing:

| Check | Result |
|---|---|
| Registration with password: alice over UDP, bob over TCP | both `OK`, the registrar saved both |
| Wrong password | `401 Unauthorized`, refused |
| G.711 (PCMU) call, dialled and answered over the control socket | bob heard 440 Hz, alice heard 1000 Hz |
| G.722 call | the same |
| G.722 call with bob on `t48audio` and no audio service running | established and closed cleanly; alice heard silence, as she should |
| G.722 call with **alice as the ARM binary under qemu-arm** | bob heard 440 Hz, alice heard 1000 Hz |

## What's left for the phone

1. Copy the ARM binary, `t48-sip`, the config and a settings file to the phone under our kernel and register to a real SIP server; dial out and in from an SSH session with `sip/test/far.py` on the control socket. The null backend is enough for this.
2. Measure CPU for a G.722 call on the 500 MHz ARM11 (expected small; G.711 is trivial).
3. Done off the phone: our control module on a Unix socket (`t48ctrl`), and the shell's client for it ([phone.md](phone.md)).
4. Done off the phone: the real `t48audio` and the audio service, tested with calls between two of each (`sip/test/audiod-test.sh`, results in [audiod.md](audiod.md#tests-and-results)). On the phone it waits for playback in the kernel driver.
5. A runit service for `t48-sip`. (Buildroot packages: done, #16.)
6. Decide on TLS (2.4 MB) and SRTP later.

## Licences

baresip and libre are BSD-3-Clause, libg722 BSD-2-Clause and public domain, mbedTLS and OpenSSL Apache-2.0. Their sources are used unmodified apart from the one patch above, and their licence texts are installed with the binary. Our files in `sip/` are AGPL-3.0-or-later.
