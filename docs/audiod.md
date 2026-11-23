# The audio service: t48-audiod

Issue [#24](https://github.com/M0LTE/yealink-t48s/issues/24). Design: [architecture.md](architecture.md#audio-service). Hardware: [audio.md](audio.md). Files in [`audiod/`](../audiod), baresip's side in [`sip/modules/t48audio/`](../sip/modules/t48audio).

## Summary

- `t48-audiod` is the one program that touches the audio hardware. It carries the call's audio to and from baresip, picks the route (handset, speakerphone, headset), sets the volume, mutes, makes every tone and the ringer in software, and controls echo on the speakerphone. The shell tells it what to do over a small control socket.
- **Tested off the phone, with nothing played anywhere.** Unit tests, a test of the real daemon over its sockets, and real calls: a sound file as one phone's microphone goes through its audio service, baresip, SIP and RTP to a second baresip and its audio service, and the same back. That passes in G.722 and G.711, and with one side running the phone's own ARM binaries under qemu.
- **Not yet run on the phone.** The hardware backend is written but has never been started, and the kernel driver cannot play sound yet. What the driver needs is [listed below](#what-the-kernel-driver-must-add). Which input and output is which is still a guess, kept in [one table](#the-route-table) that a text file can override.
- **Cost:** about 0.1 ms of CPU in each 20 ms at the very worst, estimated; well under 1% of the phone's processor. All integer.
- **Echo:** nothing for handset and headset. The speakerphone uses voice switching (whoever is talking gets the channel), which costs almost nothing. [Reasons below](#echo).

## What it does

```
                  control socket (the shell: route, volume, mute, tones, ring, keys)
                         |
 baresip  <-- call -->  t48-audiod  <-->  backend: null | file | alsa (the phone's codecs)
 (t48audio)  socket      one thread, one 20 ms tick
```

Each 20 ms tick, driven by the hardware's own sample clock:

1. take a microphone frame from the hardware;
2. take the loudspeaker frame baresip sent in answer to the last tick, if it came;
3. loudspeaker = call audio at the route's volume, plus any tone or key sound;
4. microphone = DC removed, echo control on the speakerphone, silence if muted or on hook;
5. play the loudspeaker frame, send the microphone frame to baresip.

When nothing is wanted (on hook, no call audio, no tone) the hardware is closed and the process sleeps. It uses no CPU at all while the phone is idle.

Rules worth knowing:

- **On hook means the microphone is off.** With no route set, baresip gets pure silence and call audio is not played, whatever baresip is doing.
- **The ring melody can never reach an earpiece.** It only plays with no route set, which means the loudspeaker. If a call is already up, "ring" becomes the quiet call waiting beep on the current route.
- **Tones and key sounds follow the route** and its volume. On hook (on-hook dialling) they come from the loudspeaker at the ringer volume.
- **It keeps no settings.** The shell owns the saved volumes and sends them at start. If the shell restarts, the service carries on as it was and the call is not disturbed.

## Build, run, test

```
make -C audiod                # this PC: audiod/build/t48-audiod and t48-audioctl
make -C audiod check          # unit tests and the socket test
make -C audiod bench          # cost of one frame
make -C audiod CC=$HOME/src-build/t48s/bin/armcc O=build-arm    # the phone: static ARM, 133 KB
sip/test/audiod-test.sh       # real calls; in the sip/Dockerfile container (see sip.md)
```

```
t48-audiod --backend alsa                         # the phone
t48-audiod --mic in.wav --spk out.wav             # a PC: files for microphone and loudspeaker
t48-audiod                                        # null: silence in, sound thrown away
    --run-dir DIR      where call.sock and ctl.sock go (default /run/t48-audiod)
    --routes FILE      lay a routes file over the built-in table; --print-routes shows the result
```

The default backend is null, so starting it by accident on a PC or the phone makes no sound. `t48-audioctl` speaks the shell's protocol from a prompt: `t48-audioctl route handset tone dial`, `t48-audioctl volume ringer 9`, `t48-audioctl` alone prints the state.

## The call stream (baresip)

The contract proposed in #25 is accepted with three changes. It is written down in [`t48audio_proto.h`](../sip/modules/t48audio/t48audio_proto.h), now version 2.

- Unix socket `/run/t48-audiod/call.sock`, root only. Messages are `u16 type, u16 len, payload`, like the app protocol.
- 16 kHz, mono, signed 16-bit. **The service is the clock:** it sends one MIC frame every 20 ms and baresip answers each at once with one SPK frame. Both directions run off the codec's sample clock, so nothing drifts.
- Routes, volume, mute, ringing and tones are not on this socket. baresip never knows which route is live.

What changed:

1. **Frames are always 20 ms**, whatever packet time the codec uses. baresip already re-cuts audio for the codec, so the service does not need to know.
2. **Each frame carries a sequence number** and the answer echoes it. It costs four bytes and lets the service count late answers.
3. **Either side can be missing.** If the service is not there, baresip's module keeps its own clock, feeds silence and retries twice a second, so a service restart costs a moment of silence and not the call. If baresip restarts, its new connection replaces the old one.

How late answers are handled: a tick with no SPK frame plays silence. If that leaves a spare frame queued (20 ms more delay), it is dropped after a second. Sound is delayed by 40 ms of playback buffer on the phone; the microphone by one 20 ms frame.

## The control socket (the shell)

For the shell to adopt: [`audiod/include/t48_audio_ctl.h`](../audiod/include/t48_audio_ctl.h) is the whole protocol, with no other dependencies. Unix socket `/run/t48-audiod/ctl.sock`, root only, same message framing as the app protocol, so the shell's existing reader works.

| Shell sends | Payload | |
|---|---|---|
| HELLO | `u16 version` (1) | first; answered with STATE |
| ROUTE | `u8`: 0 none, 1 handset, 2 speaker, 3 headset | none = on hook |
| VOLUME | `u8 which` (1 handset, 2 speaker, 3 headset, 4 ringer), `u8 level` 0 to 15 | 3 dB a step, 0 is silent |
| MUTE | `u8 on` | microphone only; cleared by ROUTE none |
| TONE | `u8`: 0 off, 1 dial, 2 ringback, 3 busy, 4 error, 5 call waiting | plays until replaced or stopped |
| RING | `u8 on` | an incoming call is alerting |
| KEY | `u8 character` | DTMF sound for `0-9 * #`, a click for anything else; for the owner's ear only, baresip sends the real DTMF |
| GET | none | answered with STATE |

The service sends STATE (route, mute, tone, ring, whether baresip's call stream is up, the four volumes, what the hardware can do) in answer to HELLO and GET and whenever any of it changes, and ERROR for a message it refuses.

What the shell does with it:

- **At start:** HELLO, the four saved volumes, then ROUTE for where the hook is.
- **Handset lifted, or Speaker or Headset pressed:** ROUTE, then TONE dial if there is no call. First digit: TONE off. Each digit: KEY.
- **Dialling out:** TONE ringback on baresip's `CALL_RINGING`, TONE off on `CALL_ESTABLISHED`, TONE busy or error if the call fails. On hook: TONE off, ROUTE none.
- **Call coming in:** RING 1. To answer: RING 0, then ROUTE, then tell baresip to accept. (In that order, so the answering owner never hears a call waiting beep.) Rejected or missed: RING 0.
- **Volume keys:** VOLUME for the route in use, or the ringer when on hook; the shell saves the number.

Tones are the UK plan (dial 350+440 Hz; ringback 400+450 Hz in the double-ring rhythm; busy 400 Hz, 0.375 s on and off; error 400 Hz steady). The ringer is two notes swapped 20 times a second in the same double-ring rhythm. Another country is another small table in `tone.c`.

## The route table

A route is: which codec to record from, which to play to, the echo control, and a list of mixer controls to set. The built-in table (`routes.c`) is "Reading 1" from [audio.md](audio.md): handset microphone on codec A input A, headset on codec A input B, base microphone on codec B input B, loudspeaker on codec B output B. **It is a guess until the daytime test.** Every route says so in the log until marked confirmed.

A routes file replaces any of it without a rebuild; `t48-audiod --print-routes` prints the table in the same form to start from:

```
[speaker]
capture 1              # ALSA device to record from: 0 codec A, 1 codec B
playback 1
echo switch            # or: none
coupling 256           # how loud the loudspeaker is at the microphone, 256 = the same
confirmed 1
set Codec B Capture Switch = 1
set Codec B Capture Source = 1
...
```

There are four rows: handset, speaker, headset, and alert (the loudspeaker for ringing and on-hook tones, with both microphones switched off).

## Echo

**Handset and headset: no echo control.** The earpiece hardly reaches the microphone. What they do want is sidetone (a little of your own voice in your ear), and the codec has that in hardware with no delay; software sidetone would arrive 60 ms late and sound like an echo. So sidetone is a mixer control in the route table.

**Speakerphone: voice switching.** Whichever side is talking gets the channel:

- while the far end talks, the microphone is turned down 36 dB and replaced with quiet noise at the room's background level, so the line does not sound dead;
- when the owner talks over it (louder than the echo could be), the microphone opens within 40 ms and the loudspeaker drops 15 dB;
- gains move over 10 to 40 ms so nothing clicks; the microphone stays down about 0.4 s after the far end stops, to cover the room's reverberation.

The service sees each loudspeaker frame about 40 ms before it leaves the loudspeaker, so the microphone is already down when the echo arrives.

Why this and not an echo canceller:

| | Voice switching (chosen) | Echo canceller (speexdsp, fixed point) |
|---|---|---|
| CPU on the phone | about 0.02 ms per frame, 0.1% (measured on a PC, scaled) | not measured. By counting its work (some 25 FFTs of 640 points a frame for a 200 ms tail, plus the filter update) it is in the region of 20 to 40% of this processor, before the noise suppressor it needs alongside |
| Tuning | one number per route (how loud the loudspeaker is at the microphone) | must adapt to the room, the plastic case and a small loudspeaker that distorts; can only be tuned on the phone, by ear |
| Both talking at once | one side is turned down: the classic speakerphone feel | both heard, when it works |
| When it goes wrong | someone is hard to interrupt | the far end hears themselves |

The phone has one 500 MHz core with no vector unit, and G.722 and the screen already share it. Voice switching is certain to fit, cannot howl, and could be tested properly off the phone; a canceller is none of those today. It sits behind one function (`echo_process`), so a canceller can replace it later if a measurement on the phone says there is room. The number to set on the phone is `coupling` in the routes file: too low and the far end hears echo, too high and the owner cannot break in.

Measured with a made-up room (the microphone hears the loudspeaker 60 ms late at half level): in the unit test the echo sent back is 35 dB down; in a real G.722 call through both baresips, alice hears herself back 6 dB down with no echo control and 42 dB down on the speakerphone route.

## CPU

`make -C audiod bench`, on a PC, built without vector instructions and scaled by the same integer benchmark that was run on the phone (`native/cpubench.c`, issue #9; this PC is about 19 times faster):

| Per 20 ms frame | On the phone (estimate) | Share of the CPU |
|---|---|---|
| Handset call | 8 microseconds | 0.04% |
| Speakerphone call | 24 microseconds | 0.12% |
| Worst case: speakerphone, a tone and a key sound together | 78 microseconds | 0.4% |
| Ringing | 17 microseconds | 0.08% |

That is the sample work. Each tick also makes about seven system calls (poll, read, write, the socket both ways, the clock), a few microseconds each on this processor. The scaling is rough, perhaps a factor of two or three either way, so call it **under 1% of the CPU, to be measured**. The same bench builds for ARM (`build-arm/bench`) and prints real figures on the phone. No floating point anywhere in the service; the ARM binary is 133 KB static.

## What the kernel driver must add

The service's phone backend (`hw_alsa.c`) uses the same plain ALSA ioctls as `t48rec` and `t48mix`. It works with today's capture-only driver for the microphone half: a missing playback device is reported once and it carries on. For sound out, `kernel/drivers/t48s_audio.c` needs:

1. **A playback stream on each codec:** `hw:0,0` and `hw:0,1` gain a playback side (DMA0 channels 2 and 4). S16_LE, mono, 16000 Hz, periods in multiples of 80 frames, like capture. The service asks for 320-frame periods, four of them, writes with ordinary non-blocking `write()`, sets a start threshold of 640 frames, and expects `EPIPE` on underrun (it then prepares and starts again). No mmap needed.
2. **Silence, not noise, around the edges:** nothing audible on open, start, stop, drop or underrun. That is the ramp-down the vendor driver does (`ApmHw_AmpDacRampDown`) and zeros in the FIFO when the buffer runs dry.
3. **Capture and playback of both codecs on one clock** (they are, in hardware), and capture still polls and reads as now while playback runs.
4. **Mixer controls for the output side,** with these names (they are what the route table sets; other names are fine if the table is changed to match):
   - `Codec A Output A Playback Switch`, `Codec A Output B Playback Switch`, `Codec B Output A Playback Switch`, `Codec B Output B Playback Switch`: the output mode and mute bits (registers 0x134, 0x138);
   - `Codec A Playback Volume`, `Codec B Playback Volume`: DAC digital gain (0x2c, 0x5c). The service does volume in software for now and wants these left at a fixed sensible level; later the top steps could move here for less noise at low volume;
   - `Codec A Sidetone Switch`, `Codec B Sidetone Switch`, and a `Sidetone Volume` each (0x24, 0x54);
   - `Speaker Amp Switch`: GPIO31, the external amplifier, unless the driver ties it to the loudspeaker output itself.
5. **Take over sidetone and the outputs at start-up:** today codec B idles with its microphone fed through to a live output, left that way by Yealink (the trap described in audio.md). The driver should start with every output off and sidetone off, so that nothing is heard until a route asks.
6. **Cold start** (audio.md step 4) is separate and only matters for the live image.

Nothing else: no rate other than 16 kHz, no volume ramps, no routing logic. Routes are lists of controls in user space.

## Tests and results

Run on 2026-11-23. Nothing below touched the phone or any sound device.

`make -C audiod check`: 634 checks in six unit tests, and 31 in the socket test, all passing.

| Test | What it shows |
|---|---|
| `test_tone` | every tone is the right pitch and level, the rhythms are right to 5 ms, nothing clicks on or off, key sounds end by themselves, all 12 DTMF pairs |
| `test_dsp` | volume steps are 3 dB, mixing clips and never wraps, a 9000-count DC offset is removed and a voice tone is not |
| `test_pace` | the loudspeaker queue when baresip is on time, late, bursty or too fast; the clock gives 50 frames a second, catches up a short stall and does not replay a long one |
| `test_echo` | far-end echo cut 35 dB including its first and last 60 ms; the owner alone passes untouched; break-in works within 40 ms; background noise never switches anything |
| `test_engine` | on hook the microphone is silent and the call is not played; mute; per-route volume; the ring melody stops the moment a route is set |
| `test_msg` | messages whole, a byte at a time, oversized, and to a peer that has stopped reading |
| `socket_test.py` | the real daemon with a stand-in for baresip: 100 frames arrive 20.0 ms apart, numbered from 0; silence on hook, the microphone file off hook; three late answers are played as silence and counted; an old-version client is refused; tones, ringer and key sounds land in the loudspeaker file and the hardware goes idle after each |

`sip/test/audiod-test.sh`, in the build container on another machine, through a throwaway Kamailio with password login:

| Call | Result |
|---|---|
| G.722, both sides on their own audio service | each heard the other's file all through (5975 of 6000), neither heard itself; alice heard dial tone then ringback, bob's ringer rang and stopped on answer; while alice was muted bob heard nothing; 371 frames each way, none late, none dropped |
| G.711 (through baresip's resampler) | the same |
| Echoing room, handset route | alice hears herself back 6 dB down (no echo control, as intended) |
| Echoing room, speakerphone route | alice hears herself back 42 dB down |
| G.722 with alice as the phone's ARM `t48-audiod` and ARM baresip under qemu-arm | the same checks pass, none late |

The older `sip/test/call-test.sh` still passes; its `t48audio` call now proves the fallback (no audio service running, the call still completes in silence).

## What needs the phone, and Tom

1. **Daytime test 1 and 2 of audio.md** (which input and output is which): fills in the route table.
2. **Playback in the kernel driver**, as listed above, with Tom listening.
3. **First start of `--backend alsa` on the phone, in the daytime.** Even with capture only it writes codec B's microphone controls, and until the driver takes over sidetone that can be faintly heard. Then check the log for overruns and run `build-arm/bench` for the real CPU figure.
4. **Speakerphone tuning by ear:** microphone gain, then `coupling`, then whether 36 dB and 15 dB feel right.
5. **Sidetone level** on the handset.

## Not done

- The playback stream for apps (silenced during calls) that the architecture mentions. Message types are free for it on both sockets.
- A runit service. The Buildroot package is done (#16): `t48-audiod` and `t48-audioctl` are in the image, not started at boot.
- (Done since, in #26: the shell's client, `shell/audioc.c`. See [phone.md](phone.md).)
- Tone plans for other countries, and a choice of ring melodies.
