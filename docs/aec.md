# A real echo canceller for the speakerphone

Issue [#35](https://github.com/M0LTE/yealink-t48s/issues/35). The audio service: [audiod.md](audiod.md). Files: [`audiod/aec.c`](../audiod/aec.c), [`audiod/echo.c`](../audiod/echo.c), [`audiod/third_party/speexdsp/`](../audiod/third_party/speexdsp), the comparison in [`audiod/aecbench/`](../audiod/aecbench).

## Summary

- **One fits.** speexdsp's fixed-point echo canceller costs 16.5% of the phone's processor for a 120 ms tail, measured on the phone. It is in `t48-audiod` as `echo aec`, **off by default**: the speaker route still says `echo switch` until Tom has heard it.
- **What it buys:** both sides can talk at once. On the test bench, with the far end loud, voice switching sends the owner's voice 36 dB down ("hushed", as Tom heard). With `echo aec` it goes out at full level, with the echo under it about 17 dB down, and the loudspeaker dips 6 dB instead of 15.
- **What it keeps:** with only the far end talking, the far end hears as little of itself as today. The switch is still there behind the canceller and makes up whatever the canceller has not removed, so before the canceller has learned the room (the first second or two of a call) and whenever it is doing badly, it behaves as voice switching does now.
- **Not chosen:** WebRTC's AECM is cheaper (9.4%) but turns the owner down 12 to 15 dB when both talk, which is most of what we wanted rid of. speexdsp's own preprocessor as the residual suppressor works well but takes the total to 26%.
- **Not proven:** quality was measured against a made-up room built from the phone's measured figures, not the real one. The real test is Tom's ear on a two-way call. [What to listen for](#what-to-listen-for).

## Trying it

A routes file with two lines, and the service started with it:

```
[speaker]
echo aec
```

```
t48-audiod --backend alsa --routes /tmp/aec.routes
```

`echo switch` in the same place (or no file) is today's behaviour. Add `--record /tmp/call.wav` to keep what the microphone sent, what the far end sent and what was played, for tuning afterwards.

## The candidates

| | What it is | Licence |
|---|---|---|
| speexdsp canceller (`mdf.c`) | Learns the loudspeaker-to-microphone path as a filter and subtracts the echo. Fixed point, its own integer FFT. Tail length is a setting. | BSD 3-clause |
| speexdsp preprocessor | Noise suppressor that can also push down what the canceller leaves. Runs after it. | BSD 3-clause |
| WebRTC AECM | The mobile canceller from WebRTC, fixed point. Does not subtract: it estimates how loud the echo is in each band and turns those bands down. No tail setting (it finds the delay itself, up to 400 ms). Taken from webrtc-audio-processing 0.3.1, the last release with it in plain C. | BSD 3-clause plus a patent grant |
| Plain NLMS | The textbook method, one filter tap per sample of tail, written here. For its cost. | ours |

All are compatible with AGPL-3.0-or-later. WebRTC's newer canceller (AEC3) is floating point and far too heavy for a processor with no FPU; it was not tried.

## CPU, measured on the phone

`aeccpu` (in `audiod/aecbench`), copied to `/tmp` and run with no sound: every candidate is fed the same made-up call and each 20 ms frame is timed. Median of 250 frames, phone otherwise idle, 16 kHz, static ARMv6 soft-float build (2026-11-23).

| Candidate | 64 ms tail | 128 ms | 200 ms |
|---|---|---|---|
| Voice switching (today) | 0.2% | | |
| speexdsp canceller | 14.7% (2.9 ms a frame) | 16.9% (3.4 ms) | 19.6% (3.9 ms) |
| speexdsp canceller + preprocessor | 23.7% | 26.0% | 28.6% |
| speexdsp canceller, 10 ms blocks | 15.4% | 19.9% | 25.6% |
| WebRTC AECM | 9.4% (1.9 ms), no tail setting | | |
| Plain NLMS | 75% | 148% | 229% |
| **`echo aec` as built** (speexdsp canceller, 120 ms, and the switch) | **16.5% (3.3 ms)** | | |

The old estimate of 20 to 40% for speexdsp was too high: it needs under ten FFTs a frame, not 25. Most of its cost is those FFTs, which is why the tail length matters so little. speexdsp's hand-written ARM multiply macros (`ARM5E_ASM`) made no difference: the compiler already does as well. Starting the canceller at a route change takes under 10 ms.

With audiod's own 2.6% and the kernel side, a speakerphone call's audio would be about 19% before baresip and G.722.

## Quality, on a made-up speakerphone

`aecq` (same directory, `make quality`) on a PC. It cannot be the real room, so it is built from what was measured on the phone ([audio.md](audio.md#playback-2026-11-23-issue-24)): sound reaches the microphone 45 ms after the service hands it over; the path is loudest at 1.4 kHz (+7 dB against what was played) and 10 dB down at 400 Hz; a room follows the direct sound and dies away over 0.35 s, longer than any tail tried; the small loudspeaker distorts when loud; the microphone has noise at -49 dBFS and the owner, at arm's length, is at -34 dBFS. Speech is two CMU Arctic speakers. The call is 40 s: far end alone, both talking, far end alone, owner alone, then the room changes (something put down by the phone).

Far end loud (its echo at the microphone is 5 dB above the owner), all in dB:

| | Echo reduction, far end alone, settled | first 2 s of the call | first 2 s after the room changes | Both talking: owner's voice | Both talking: echo and other residue, below the echo | Loudspeaker while both talk |
|---|---|---|---|---|---|---|
| Voice switching (today) | 27 | 22 | 19 | **-36** | (owner not sent) | 0 |
| speexdsp canceller alone, 64 ms | 13 | 6 | 7 | -0.6 | 10 | 0 |
| speexdsp canceller alone, 128 ms | 16 | 5 | 7 | -0.6 | 12 | 0 |
| speexdsp canceller alone, 200 ms | 16 | 5 | 7 | -0.6 | 12 | 0 |
| speexdsp canceller + preprocessor, 128 ms | 24 | 10 | 11 | -1.0 | 13 | 0 |
| WebRTC AECM | 20 | 14 | 21 | **-12.5** | 12 | 0 |
| **`echo aec` as built** | **22** | **22** | **12** | **-0.6** | **12, and the loudspeaker's 6 more** | -5 |

- The room's noise sets the ceiling here: at -49 dBFS it is only about 20 dB under the echo, so anything that sends comfort noise in place of the microphone reads 22 to 27. With a quiet room and a clean loudspeaker the same table reads 35 for switching, 26 for `echo aec`, 22 for the canceller alone.
- At middle volume the echo is under the owner anyway and every method lets the owner through; `echo aec` and switching both read 16.
- The canceller alone is not enough: it is slow to learn (5 dB in the first 2 s) and stops at 16 dB because the loudspeaker distorts and the room rings on past its tail. Longer than about 128 ms buys nothing for that reason. That is why the switch stays behind it.
- AECM is quick and steady but is half-duplex in practice: it turns the owner down 12 to 15 dB when both talk.
- The weak spot of `echo aec`: for a second or two after something moves near the phone, the far end may hear a little of itself (12 dB down instead of 19), until the canceller relearns.

Full tables for all three conditions and every tail: `make quality`. `aecq DIR -w OUTDIR` also writes each candidate's result as a WAV (left: what the far end hears; right: the loudspeaker) to listen to.

## What was built

`echo aec` is the canceller with the existing switch behind it (`echo.c`):

1. speexdsp's canceller subtracts its estimate of the echo from the microphone. Its reference is what was played, one frame (20 ms) late, because the sound cannot arrive sooner than that; with a 120 ms filter that covers the first 140 ms of the path.
2. The canceller also reports how much of the echo it thinks is left. The switch uses that two ways:
   - **Depth.** While only the far end talks, the microphone is cut by just enough to bring the total to 36 dB, the same as today. Once the canceller is doing its 16 to 23 dB that is a cut of 13 to 20 dB, not 36, so the owner's small noises are not chopped out so hard.
   - **Breaking in.** The owner is judged against the echo that is left, not all of it. So the owner gets the channel even when quieter than the echo, and because the canceller is still working under the owner's voice the loudspeaker only drops 6 dB.
3. Until the canceller says it has learned the path, and whenever it says it is leaving everything in, both of those fall back to exactly today's voice switching. If there is no memory for the canceller the route is plain `echo switch`.

No floating point on the audio path. The ARM binary grows from 133 KB to 161 KB. Unit test: `audiod/tests/test_aec.c` (in `make check`), which also shows the same call defeating voice switching alone.

speexdsp is vendored in `audiod/third_party/speexdsp` with its licence; `README.t48` there says what was taken and the one function added.

## What to listen for

On a real two-way call on the speakerphone, with someone at the far end who will say what they hear:

1. **Talk over them.** Count aloud while they talk. They should hear you at normal level throughout, not hushed or chopped. You should hear them dip a little, not vanish.
2. **Ask whether they hear themselves.** Worst at the start of the call and at high volume. A faint, short echo in the first second or two is the canceller learning; echo that stays is a fault.
3. **Move things.** Put a hand or a book next to the phone while they talk. They may hear a moment of echo; it should go within a couple of seconds.
4. **Full volume.** The loudspeaker distorts most there and the canceller does least. If the far end hears echo only at the top few volume steps, that is this.
5. **Your own background.** While they talk they hear soft noise in place of your room; it should not pump or sound like a different room.

If it is wrong, `--record` the call and the three tracks say which part misbehaved.

## Not done

- No measurement of the real loudspeaker-to-microphone path. A `--record` of a call with `echo none` at moderate volume would give it, and `aecq` could then replay the real room instead of the made-up one.
- The 20 ms reference delay assumes playback is at least one frame behind, which is how `hw_alsa.c` runs it (it starts with 40 ms queued). If that buffering is ever shortened the delay must go with it.
- speexdsp's preprocessor would add noise suppression and a few dB, for another 9% of the processor. Not taken: it does not fit the budget alongside G.722 and the screen.
- A cheaper canceller (lower band only, or 8 kHz) was not tried: 16.5% fits.
