# The audio service: t48-audiod

Issue [#24](https://github.com/M0LTE/yealink-t48s/issues/24). Design: [architecture.md](architecture.md#audio-service). Hardware: [audio.md](audio.md). Files in [`audiod/`](../audiod), baresip's side in [`sip/modules/t48audio/`](../sip/modules/t48audio).

## Summary

- `t48-audiod` is the one program that touches the audio hardware. It carries the call's audio to and from baresip, picks the route (handset, speakerphone, headset), sets the volume, mutes, makes every tone and the ringer in software, and controls echo on the speakerphone. The shell tells it what to do over a small control socket.
- **Tested off the phone, with nothing played anywhere.** Unit tests, a test of the real daemon over its sockets, and real calls: a sound file as one phone's microphone goes through its audio service, baresip, SIP and RTP to a second baresip and its audio service, and the same back. That passes in G.722 and G.711, and with one side running the phone's own ARM binaries under qemu.
- **Runs on the phone (2026-11-23).** With the kernel driver's playback it plays tones and call audio through the loudspeaker and the handset, and a real call to an echo service went round the whole loop. [Results below](#on-the-phone). Which input and output is which is settled by the daytime tests and kept in [one table](#the-route-table) that a text file can override. How it sounds still needs an ear.
- **Cost, measured on the phone:** about 2.6% of the processor while a speakerphone call's audio runs, kernel side included. The sample work alone is 0.4%; the rest is waking up fifty times a second. All integer.
- **Apps can play sound** (issue [#63](https://github.com/M0LTE/yealink-t48s/issues/63)): 16 kHz mono from a ring in memory the app shares with the service, mixed in with everything else, under the loudspeaker's limit, and silent whenever the phone rings or is in a call. [Below](#an-apps-sound).
- **Echo:** nothing for handset and headset. The speakerphone uses voice switching (whoever is talking gets the channel), which costs almost nothing. [Reasons below](#echo). A real echo canceller is built in as `echo aec`, off until it has been tried by ear: [aec.md](aec.md).

## What it does

```
                  control socket (the shell: route, volume, mute, tones, ring, keys, the app's ring)
                         |
 baresip  <-- call -->  t48-audiod  <-->  backend: null | file | alsa (the phone's codecs)
 (t48audio)  socket      one thread, one 20 ms tick
                         |
 the app in front  -->  shared memory (a ring of samples; no socket)
```

Each 20 ms tick, driven by the hardware's own sample clock:

1. take a microphone frame from the hardware;
2. take the loudspeaker frame baresip sent in answer to the last tick, if it came;
3. loudspeaker = call audio at the route's volume, plus any tone or key sound, plus the app's sound if nothing above wants the loudspeaker;
4. microphone = DC removed, echo control on the speakerphone, silence if muted or on hook;
5. play the loudspeaker frame, send the microphone frame to baresip.

When nothing is wanted (on hook, no call audio, no tone, no app writing sound) the hardware is closed and the process sleeps. It uses no CPU at all while the phone is idle.

Rules worth knowing:

- **On hook means the microphone is off.** With no route set, baresip gets pure silence and call audio is not played, whatever baresip is doing.
- **The ring melody can never reach an earpiece.** It only plays with no route set, which means the loudspeaker. If a call is already up, "ring" becomes the quiet call waiting beep on the current route.
- **Tones and key sounds follow the route** and its volume. On hook (on-hook dialling) they come from the loudspeaker at the ringer volume.
- **The loudspeaker has a hard limit.** Whatever goes to its amplifier (ringer, on-hook tones, the speaker route's call audio) passes a peak limiter, and no setting or routes file gets round it. [Why, and the numbers](#the-loudspeakers-limit).
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
    --silent           alsa, for tests in a quiet house: all as usual, but the amplifier,
                       the outputs and the sidetone are never switched on
```

The default backend is null, so starting it by accident on a PC or the phone makes no sound. `t48-audioctl` speaks the shell's protocol from a prompt: `t48-audioctl route handset tone dial`, `t48-audioctl volume ringer 9`, `t48-audioctl` alone prints the state.

## The call stream (baresip)

The contract proposed in #25 is accepted with three changes. It is written down in [`t48audio_proto.h`](../sip/modules/t48audio/t48audio_proto.h), now version 2.

- Unix socket `/run/t48-audiod/call.sock`, root only. Messages are `u16 type, u16 len, payload`, like the app protocol.
- 16 kHz, mono, signed 16-bit. **The service is the clock:** it sends one MIC frame every 20 ms and baresip answers each at once with one SPK frame. Both directions run off the codec's sample clock, so nothing drifts.
- Routes, volume, mute, ringing and tones are not on this socket. baresip never knows which route is live.
- One stream however many calls baresip has (two at most on the phone, #27): its module gives the microphone frame to each call and adds what the calls play together. A call on hold plays nothing, so the service still hears one.

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
| VOLUME | `u8 which` (1 handset, 2 speaker, 3 headset, 4 ringer, 5 apps), `u8 level` 0 to 15 | 0 is silent. Handset, speaker and headset: 3 dB a step, 15 is full level. Ringer and apps: 2 dB a step, 15 is the [loudspeaker's limit](#the-loudspeakers-limit) |
| MUTE | `u8 on` | microphone only; cleared by ROUTE none |
| TONE | `u8`: 0 off, 1 dial, 2 ringback, 3 busy, 4 error, 5 call waiting | plays until replaced or stopped |
| RING | `u8 on` | an incoming call is alerting |
| KEY | `u8 character` | DTMF sound for `0-9 * #`, a click for anything else; for the owner's ear only, baresip sends the real DTMF |
| GET | none | answered with STATE |
| APP | `u8`: 0 gone, 1 off the screen, 2 in front; with a new app, its ring as a descriptor beside the message | [An app's sound](#an-apps-sound) |

The service sends STATE (route, mute, tone, ring, whether baresip's call stream is up, the volumes, what the hardware can do, where the app is and whether its sound is playing) in answer to HELLO and GET and whenever any of it changes, and ERROR for a message it refuses.

What the shell does with it:

- **At start:** HELLO, the four saved volumes, then ROUTE for where the hook is.
- **Handset lifted, or Speaker or Headset pressed:** ROUTE, then TONE dial if there is no call. First digit: TONE off. Each digit: KEY.
- **Dialling out:** TONE ringback on baresip's `CALL_RINGING`, TONE off on `CALL_ESTABLISHED`, TONE busy or error if the call fails. On hook: TONE off, ROUTE none.
- **Call coming in:** RING 1. To answer: RING 0, then ROUTE, then tell baresip to accept. (In that order, so the answering owner never hears a call waiting beep.) Rejected or missed: RING 0.
- **Volume keys:** VOLUME for the route in use; on hook the ringer, or apps while an app with sound is in front. The shell saves the number.
- **An app with the `audio-out` permission starts:** APP 2 with its ring. APP 1 the moment it is paused, 2 when it is back, 0 when it has gone. Sent again, with the ring, if the service restarts.

Tones are the UK plan (dial 350+440 Hz; ringback 400+450 Hz in the double-ring rhythm; busy 400 Hz, 0.375 s on and off; error 400 Hz steady). The ringer is two notes (1000 and 1250 Hz) taking turns 20 times a second in the same double-ring rhythm; each note is a sine that fades into the other over 6 ms, and each burst fades in and out over 20 ms. Every tone starts and stops on a half-cosine fade. Another country is another small table in `tone.c`.

## An app's sound

Issue [#63](https://github.com/M0LTE/yealink-t48s/issues/63). The app's side is in [app-protocol.md](app-protocol.md#sound); `appsnd.c` is the service's.

**Transport: a ring of samples in shared memory.** The shell makes a memfd (4096 bytes of header, then 4096 samples), gives one descriptor to the app when it starts it and one to the service with APP. The app writes samples and moves `wr`; each tick the service copies one frame out and moves `rd`. That one copy is all there is: no socket, no system call for either side while sound flows. If the app is asleep waiting for room, the service wakes it with a futex after a take (one system call, only then). Format: 16 kHz, mono, 16-bit, the codecs' own, so nothing is resampled here.

**The app is not trusted, and the service runs as root at real-time priority.** So:

- the memfd is sealed against changing size, and the service refuses a ring that is not: reading it can never fault;
- the service believes nothing in the ring but `wr`. Its own count of what it has taken is kept outside, and the ring's size and layout are constants. A `wr` that cannot be right (further ahead than the ring is long) empties the ring;
- the app has no socket to the service. The control socket stays root only.

**Rules:**

- **Where.** On hook: the loudspeaker (the alert row of the route table, microphones off). If a route is set with no call on it, that route; the shell does not do this today, since lifting the handset or pressing Speaker or Headset starts a call and pauses the app.
- **How loud.** Its own volume (5, "Apps"), 0 to 15, 2 dB a step. On the loudspeaker a full-scale sample at 15 is exactly on the [limit](#the-loudspeakers-limit) (1800), so the limiter never has to squash an app's sound by itself; it is still there for an app with a key sound on top. In an earpiece the scale is the call's (3 dB a step, 15 is full scale).
- **Calls win.** While RING is on, a tone is playing, or a route is set with the call stream up, the app's samples are taken at the same rate and not played, and the ring's `state` says held. The change is a 20 ms fade each way, so nothing clicks. A key sound does not hold it.
- **Off the screen is silent.** APP 1 drops what is waiting and nothing more is taken until APP 2.
- **An app that stops writing is silence.** A frame is taken only if it is there; a part frame is played out and padded. Nothing is ever played twice.
- **The hardware.** It runs, amplifier on, the whole time an app with sound is in front, so a game's next beep starts at once. When the app leaves or is paused, it idles 2 s later.

**Delay.** A sample waits in the ring for the next tick (10 ms on average), then behind the 20 ms the playback buffer holds, then 5 ms in the driver: 35 ms, plus whatever the app keeps waiting in the ring so as never to run out. Measured on the phone with the example app, which writes 10 ms at a time and keeps 640 samples at most: 40 ms in the ring on average, and the playback buffer held exactly 640 samples after every write, so 65 ms from a write to the loudspeaker. The floor for an app that keeps 480 waiting is about 50 ms. The 20 ms tick and the 40 ms the playback buffer starts with are what the call path was built on, and were left alone.

**Cost.** One more source to mix: about 5 microseconds a frame on the phone by the bench's scale. The clock is the cost, as for anything else that makes sound: 2.6% of the processor while it runs. libt48's resampler, measured on the phone: 0.55% of the processor for 48 kHz stereo, 0.51% for 44.1 kHz, 0.26% for 22.05 kHz.

## The route table

A route is: which codec to record from, which to play to, the echo control, and a list of mixer controls to set. The built-in table (`routes.c`) follows the daytime tests in [audio.md](audio.md): the handset is codec B (input B, output B), the headset is codec B (input A, output A), and the speakerphone is codec A (input A, output A) with the amplifier. The gains are the ones Yealink's firmware sets in each mode:

| Route | Microphone | Out |
|---|---|---|
| handset | preamp 15 dB, ADC 0 dB | DAC +3 dB, output B -9 dB, sidetone on |
| headset | preamp 21 dB (Yealink 22; the control moves in 3 dB steps), ADC 0 dB | DAC +3 dB, output A -6 dB, sidetone on |
| speaker | preamp 21 dB, ADC +12 dB (Yealink +1 dB: the base microphone is quiet at arm's length and Yealink makes it up in its DSP) | DAC -6 dB, output A -3 dB, amplifier on |
| alert | both off | as speaker |

The sidetone level (-18 dB) is our choice, not Yealink's, and wants setting by ear. The headset row has not been tried with a headset plugged in, and says so in the log until marked confirmed.

Each list is in an order that stays silent: amplifier and the other outputs off first, then gains, then this route's output, then the amplifier. When the service goes idle it switches the amplifier, all outputs, both sidetones and both microphones off.

A routes file replaces any of it without a rebuild; `t48-audiod --print-routes` prints the table in the same form to start from:

```
[speaker]
capture 0              # ALSA device to record from: 0 codec A, 1 codec B
playback 0
echo switch            # or: none, or aec (the echo canceller, aec.md)
coupling 512           # how loud the loudspeaker is at the microphone, 256 = the same
confirmed 1
set Codec A Capture Switch = 1
set Codec A Capture Source = 0
...
```

There are four rows: handset, speaker, headset, and alert (the loudspeaker for ringing and on-hook tones, with both microphones switched off).

A route that switches the amplifier on cannot set the loudspeaker's DAC gain above -6 dB or its output gain above -3 dB. A routes file that asks for more is told so in the log and gets those values.

## The loudspeaker's limit

Issue [#57](https://github.com/M0LTE/yealink-t48s/issues/57): a loud ring froze the phone, and the watchdog reset it 30 seconds later. Nothing is logged.

**The cause is the phone's power budget, and the ringer is only one part of it.** Screen brightness, the four lamps, a busy processor and the loudspeaker add up, and past a point the phone freezes. Measured on 2026-11-24 with the old ringer (1 kHz and 1.25 kHz at a steady level), raising the level 1 dB at a time, on the OEM power adapter:

| Screen | Lamps | Processor | Sound | Result |
|---|---|---|---|---|
| lit | off | idle | old ringer at -16.2 dBFS peak | fine |
| lit | off | idle | old ringer at -15.2 dBFS | freeze (Linux 3.6) |
| full | all 4 on | idle | old ringer at -18.2 dBFS | fine |
| full | all 4 on | idle | old ringer at -17.2 dBFS | freeze, on Linux 3.6 and on 6.18 |
| full | all 4 on | 60% busy | new ringer at the limit, -25.2 dBFS | freeze within 20 s |
| full | all 4 on | fully busy | no sound, amplifier on | fine for 10 s |
| full | all 4 on | fully busy | any sound at all, even a quiet dial tone in the handset | freeze within 20 s |
| full | off | fully busy | quiet dial tone in the handset | fine for 40 s |
| at 1 of 10 | all 4 on | fully busy | quiet dial tone in the handset | fine for 40 s |

The freeze reported by hand was at -21.2 dBFS (the old ringer at volume 10), lower than any of ours with an idle processor.

**What the audio service does about its share:**

- A peak limiter on every route that uses the amplifier: no sample leaves above 1800 (-25.2 dBFS). That is 8 dB (a sixth of the power) under the level that froze the phone with the screen full and every lamp lit, and 4 dB under the report. A louder sound is turned down within one sample and let back up over about 60 ms, so a tone stays a tone; it is not clipped.
- The ringer's volume runs up to that limit and no further: 15 is exactly on it, each step below is 2 dB, and the default is 13. The old steps were 3 dB from a level five times higher, so 10 and above could all freeze the phone.
- The loudspeaker's analog and DAC gains are held at the values the limit was measured with.
- The ringer itself is gentler: sine notes that fade into each other, bursts that fade in and out.

**Loudness, at the phone's own base microphone:** the ringer at 15 is 5 dB louder than the old ringer at 7 (the loudest setting known to be safe before); at 14 it matches the old 8; at the default, 13, it is 1 dB louder than the old 7.

**Proved:** a 20-second ring at volume 15, by a real call and with every lamp lit and the screen full, with no freeze, on both kernels ([below](#on-the-phone)).

**Not solved here:** the budget itself. With the screen full, every lamp lit and the processor busy, the phone has nothing left for any sound, in a call as much as when ringing. That needs the supply measured ([#58](https://github.com/M0LTE/yealink-t48s/issues/58)).

## Echo

**Handset and headset: no echo control.** The earpiece hardly reaches the microphone. What they do want is sidetone (a little of your own voice in your ear), and the codec has that in hardware with no delay; software sidetone would arrive 60 ms late and sound like an echo. So sidetone is a mixer control in the route table.

**Speakerphone: voice switching.** Whichever side is talking gets the channel:

- while the far end talks, the microphone is turned down 36 dB and replaced with quiet noise at the room's background level, so the line does not sound dead;
- when the owner talks over it (louder than the echo could be), the microphone opens within 40 ms and the loudspeaker drops 15 dB;
- gains move over 10 to 40 ms so nothing clicks; the microphone stays down about 0.4 s after the far end stops, to cover the room's reverberation.

The service sees each loudspeaker frame about 40 ms before it leaves the loudspeaker, so the microphone is already down when the echo arrives.

**Since issue #35 there is a canceller too**, as `echo aec` in the routes file, off by default. It was measured on the phone at 16.5% of the processor (the estimate in the table below was too high) and lets both sides talk at once; the switch described here stays behind it as its fallback. Everything about it is in [aec.md](aec.md).

Why voice switching was chosen first, as written before that measurement:

| | Voice switching (chosen) | Echo canceller (speexdsp, fixed point) |
|---|---|---|
| CPU on the phone | about 0.02 ms per frame, 0.1% (measured on a PC, scaled) | not measured. By counting its work (some 25 FFTs of 640 points a frame for a 200 ms tail, plus the filter update) it is in the region of 20 to 40% of this processor, before the noise suppressor it needs alongside |
| Tuning | one number per route (how loud the loudspeaker is at the microphone) | must adapt to the room, the plastic case and a small loudspeaker that distorts; can only be tuned on the phone, by ear |
| Both talking at once | one side is turned down: the classic speakerphone feel | both heard, when it works |
| When it goes wrong | someone is hard to interrupt | the far end hears themselves |

The phone has one 500 MHz core with no vector unit, and G.722 and the screen already share it. Voice switching is certain to fit, cannot howl, and could be tested properly off the phone; a canceller is none of those today. It sits behind one function (`echo_process`), so a canceller can replace it later if a measurement on the phone says there is room. The number to set on the phone is `coupling` in the routes file: too low and the far end hears echo, too high and the owner cannot break in. It is 512 (twice, +6 dB): measured on the phone, the base microphone hears a tone from the loudspeaker between 20 dB below and 7 dB above its level in the frame played, loudest at 1.4 kHz ([audio.md](audio.md#playback-2026-11-23-issue-24)).

Measured with a made-up room (the microphone hears the loudspeaker 60 ms late at half level): in the unit test the echo sent back is 35 dB down; in a real G.722 call through both baresips, alice hears herself back 6 dB down with no echo control and 42 dB down on the speakerphone route.

## CPU

`make -C audiod bench`, on a PC, built without vector instructions and scaled by the same integer benchmark that was run on the phone (`native/cpubench.c`, issue #9; this PC is about 19 times faster):

| Per 20 ms frame | On the phone (estimate) | Share of the CPU |
|---|---|---|
| Handset call | 8 microseconds | 0.04% |
| Speakerphone call | 24 microseconds | 0.12% |
| Worst case: speakerphone, a tone and a key sound together | 78 microseconds | 0.4% |
| Ringing | 17 microseconds | 0.08% |

That was the estimate for the sample work. **Measured on the phone (2026-11-23)** the same bench gives about three times those figures: 74 microseconds a frame for a speakerphone call (0.4%), 240 for the worst case (1.2%).

The whole cost is more than the sample work, because each tick is also an interrupt, a wake-up and about seven system calls. Measured by what a competing low-priority counting loop loses while the service runs:

| Running | Processor taken |
|---|---|
| speaker route, service and kernel driver | 2.6% |
| handset route, service and kernel driver | 2.4% |
| speaker route with a client on the call stream answering every frame (the client's own work included) | 5.6% |

So **about 2.6% for a speakerphone call's audio**, before baresip and its codec. The kernel's own per-process accounting cannot show this (it reads zero: the service runs between its clock ticks). No floating point anywhere in the service; the ARM binary is 133 KB static.

The service runs at real-time priority on the phone. Without it, one SSH login during a call was enough to make it 40 ms late: the playback ran dry and baresip's answers arrived late. With it, the same call had no late frame.

## What the kernel driver must add

The service's phone backend (`hw_alsa.c`) uses the same plain ALSA ioctls as `t48rec` and `t48mix`. With a capture-only driver it works for the microphone half: a missing playback device is reported once and it carries on. For sound out, `kernel/drivers/t48s_audio.c` needs:

All of this is in the driver since 2026-11-23, except cold start ([audio.md](audio.md#playback-2026-11-23-issue-24)). It also has an analog gain control for each output (`Codec A Output A Playback Volume` and so on), which the routes set.

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

Run on 2026-11-23. Nothing in this section touched the phone or any sound device; the phone's results follow it.

`make -C audiod check`: 851 checks in eight unit tests, and 52 in the socket test, all passing (2026-11-25).

| Test | What it shows |
|---|---|
| `test_tone` | every tone is the right pitch and level, the rhythms are right to 5 ms, nothing clicks on or off, key sounds end by themselves, all 12 DTMF pairs. The ringer: both notes at full level in their turns, never above one note's level where they overlap, a 20 ms fade at each end of a burst and when stopped, nothing but the two notes in the spectrum, no corner in the wave |
| `test_dsp` | volume steps are 3 dB, mixing clips and never wraps, a 9000-count DC offset is removed and a voice tone is not. The ringer's steps are 2 dB with 15 on the limit. The limiter: nothing under the ceiling is changed by a bit; a full-scale tone comes out at the ceiling and still a sine (third harmonic under 1%); full-scale square waves and noise from silence never pass it, at any frame size; the gain is back a quarter of a second after something loud |
| `test_pace` | the loudspeaker queue when baresip is on time, late, bursty or too fast; the clock gives 50 frames a second, catches up a short stall and does not replay a long one |
| `test_echo` | far-end echo cut 35 dB including its first and last 60 ms; the owner alone passes untouched; break-in works within 40 ms; background noise never switches anything |
| `test_engine` | on hook the microphone is silent and the call is not played; mute; per-route volume; the ring melody stops the moment a route is set. The limit: the loudest call with a tone and a key sound on top at speaker volume 15, and the ringer at every volume, never pass 1800; a quiet call and the earpiece are untouched; a routes file cannot raise the loudspeaker's gains, and a route that switches the amplifier on is limited whatever its name |
| `test_msg` | messages whole, a byte at a time, oversized, and to a peer that has stopped reading |
| `test_appsnd` | an app's sound. The ring: only a sealed memfd of the right size is taken; every sample comes out once and in order, round the ring many times; a sound starts on a whole frame and a short one is not kept waiting; an app that scribbles on the ring is emptied, not believed; off the screen nothing is taken and what waited is dropped. The engine: volume 15 is the limit and each step is 2 dB; silence when the app stops; faded out within a frame for ringing, a tone and a call, and back after; never past the limit with a key sound on top |
| `socket_test.py`, an app | the real daemon with a stand-in for the shell and an app: an unsealed ring is refused; writing starts the hardware and is taken at 16000 samples a second without running dry; a full-scale tone at volume 15 lands on the limit; while the phone rings the app's sound is taken and not played; paused, what waited is dropped and nothing is taken; the hardware idles 2 s after; when the shell's connection closes the ring is let go |
| `socket_test.py`, calls | the real daemon with a stand-in for baresip: 100 frames arrive 20.0 ms apart, numbered from 0; silence on hook, the microphone file off hook; three late answers are played as silence and counted; an old-version client is refused; tones, ringer and key sounds land in the loudspeaker file and the hardware goes idle after each |

`sip/test/audiod-test.sh`, in the build container on another machine, through a throwaway Kamailio with password login:

| Call | Result |
|---|---|
| G.722, both sides on their own audio service | each heard the other's file all through (5975 of 6000), neither heard itself; alice heard dial tone then ringback, bob's ringer rang and stopped on answer; while alice was muted bob heard nothing; 371 frames each way, none late, none dropped |
| G.711 (through baresip's resampler) | the same |
| Echoing room, handset route | alice hears herself back 6 dB down (no echo control, as intended) |
| Echoing room, speakerphone route | alice hears herself back 42 dB down |
| G.722 with alice as the phone's ARM `t48-audiod` and ARM baresip under qemu-arm | the same checks pass, none late |

The older `sip/test/call-test.sh` still passes; its `t48audio` call now proves the fallback (no audio service running, the call still completes in silence).

## On the phone

Run on 2026-11-23 in the daytime at moderate level, with the phone's base microphone as the listener. `t48-audiod --backend alsa`, the built-in routes, and `calltap` (`audiod/tests/calltap.c`) standing in for baresip on the call stream where there was no call.

| Test | Result |
|---|---|
| The service's own tone (the 400 Hz error tone, speaker route, volume 12) out of the loudspeaker and back through the base microphone to the call stream, echo control off for the measurement | played at -24.3 dBFS, arrived at -33.0 dBFS, steady within 1 dB; 350 frames each way, none late, no overruns or underruns |
| Far-end tone (1 kHz at -30 dBFS, volume 12) with the speaker route as built in | without echo control it comes back at -33.7 dBFS; with it, at -70 dBFS: 36 dB down, as designed |
| 24 s speaker-route stream | 1200 frames each way, none late, none dropped; tick 131 microseconds on average, 153 at most |
| Going idle | amplifier, outputs, sidetone and microphones all off, DMA stopped |
| A real call: registered to the test SIP server ([sip-test-server.md](sip-test-server.md)), called its echo service, speaker route at volume 10, 10 s, G.722 | 493 frames each way, none late, no overruns or underruns. What the microphone sent came back from the echo service 283 ms later at the same level (0.1 dB, correlation 0.98), and was played |

Run on 2026-11-24 for the loudspeaker's limit (issue #57), on development builds of this tree, ringing from the test SIP server:

| Test | Linux 6.18 | Linux 3.6 |
|---|---|---|
| A real call left ringing for 20 s at ringer volume 15 | no freeze | no freeze |
| The ringer at 15 for 20 s with the screen at full brightness and all four lamps lit | no freeze | no freeze |
| The same with the processor 60% busy as well | froze within 20 s | not tried |

Loudness and the levels that freeze the phone are in [The loudspeaker's limit](#the-loudspeakers-limit).

Run on 2026-11-25, at night, for an app's sound (issue #63). **Nothing was audible:** the service ran with `--silent`, and the mixer read back with the amplifier and all four outputs off each time it was looked at while sound was flowing (first with the hardware running on digital silence, before any sound was made). The phone's own shell and audio service were stopped and this tree's ARM builds run in their place from `/tmp`, with `libt48/examples/tones` as the app, keys and the hook pressed through the kernel's test hooks, and `--record` keeping what the service sent to the codec. Three runs.

| Test | Result |
|---|---|
| The four notes, twice, at Apps volume 12 | sent to the codec at 660 to 662 (a peak of 24000 at volume 12 should be 661) |
| Volume up twice | Apps volume 14 at the service, ringer untouched; a tone then sent at 1047 to 1056 (should be 1047) |
| A steady 600 Hz tone at volume 12 for 7.5 s | 660.7 in every tenth of a second, no gap |
| The ringer asked for during the tone, for 2 s | the tone gone within 0.25 s, the ringer's two notes in its place at the ringer's level, the tone back when ringing stopped; never above 1211 on the way (the limit is 1800) |
| Handset lifted during the tone | the app paused in the same moment, the service on the handset route with dial tone; handset down: the app back in front |
| Home during a tone | paused, nothing more taken; the hardware idle 2 s later |
| The longest run | 197280 samples in 619 frames, no hardware underrun, none dropped, no reset of the ring; the ring ran dry three times, each the end of a sound |
| Delay | 40 ms waiting in the ring on average (the app keeps up to 640 samples there), in all three runs; the playback buffer held 640 samples after each of 1175 writes. 65 ms from write to loudspeaker |
| Processor, during the steady tone | the service 1%, the app and the shell under 1% each (`top`) |
| Afterwards | the phone's own services started again, registered, idle, amplifier and outputs off |

Not done: hearing it. The steps are in [What needs the phone, and Tom](#what-needs-the-phone-and-tom).

## What needs the phone, and Tom

1. **A listen.** Everything above was measured with microphones. How the loudspeaker and earpiece sound, and whether the levels are right, needs an ear. An app's sound has not been heard at all yet: open Tones (`libt48/examples/tones`, in `/data/apps/tones` on the bench phone) for four rising notes; OK repeats them, 1 beeps, 2 holds a tone until pressed again, the volume keys change the level. Whether volume 15, on the limit, is loud enough for a game is the first thing to find out.
2. **Sidetone level** on the handset (`Codec B Sidetone Volume` in the handset route; -18 dB now).
3. **Speakerphone tuning by ear:** microphone gain, then `coupling`, then whether 36 dB and 15 dB feel right.
4. **The headset**, with one plugged in.
5. **baresip's priority.** The service is real-time now; baresip is not, and a busy phone could still make its side late (#25).

## Not done

- A runit service. The Buildroot package is done (#16): `t48-audiod` and `t48-audioctl` are in the image, not started at boot.
- (Done since, in #26: the shell's client, `shell/audioc.c`. See [phone.md](phone.md).)
- Tone plans for other countries, and a choice of ring melodies.
