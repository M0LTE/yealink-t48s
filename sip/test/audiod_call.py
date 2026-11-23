#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The shell's part in sip/test/audiod-test.sh: drives two baresip instances
over ctrl_tcp and their two t48-audiod services over the control socket, then
checks what each service "played" (a WAV file) for what should and should not
be in it.

  audiod_call.py make-wavs DIR
  audiod_call.py call|call-slow|echo-handset|echo-speaker DIR AUDIOCTL
"""
import math, os, struct, subprocess, sys, time, wave
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ctrl import Ctrl

ALICE_HZ, BOB_HZ, LEVEL = 700, 1100, 6000     # what each microphone "hears"
fails = 0


def check(ok, what):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        fails += 1


def amp(samples, hz):
    if len(samples) < 160:
        return 0.0
    w = 2 * math.pi * hz / 16000
    re = sum(x * math.cos(w * i) for i, x in enumerate(samples))
    im = sum(x * math.sin(w * i) for i, x in enumerate(samples))
    return 2 * math.hypot(re, im) / len(samples)


def best(samples, hz, win=3200):
    """Strongest 0.2 s of that frequency anywhere in the samples."""
    return max([amp(samples[i:i + win], hz) for i in range(0, max(1, len(samples) - win), win // 2)] or [0.0])


def worst(samples, hz, win=3200):
    return min([amp(samples[i:i + win], hz) for i in range(0, max(1, len(samples) - win), win // 2)] or [0.0])


def make_wavs(d):
    for name, hz in (("alice-mic.wav", ALICE_HZ), ("bob-mic.wav", BOB_HZ)):
        with wave.open(os.path.join(d, name), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
            # a whole number of cycles in one second, so the loop is seamless
            w.writeframes(struct.pack("<16000h", *[int(LEVEL * math.sin(2 * math.pi * hz * i / 16000)) for i in range(16000)]))


class Side:
    def __init__(self, d, name, port, audioctl):
        self.name, self.dir, self.audioctl = name, os.path.join(d, name), audioctl
        self.sip = Ctrl(name, port)
        self.sip.events = []

    def audio(self, *args):
        r = subprocess.run([self.audioctl, "-s", os.path.join(self.dir, "audio", "ctl.sock"), *args],
                           capture_output=True, text=True)
        return r.stdout.strip()

    def mark(self):
        """Where in its loudspeaker file this service is now, in samples.
        The file is written through a 4 KB buffer, so good to about 0.15 s."""
        return max(0, os.path.getsize(os.path.join(self.dir, "spk.wav")) - 44) // 2

    def played(self):
        with open(os.path.join(self.dir, "spk.wav"), "rb") as f:
            d = f.read()[44:]
        return list(struct.unpack("<%dh" % (len(d) // 2), d[:len(d) // 2 * 2]))


def main():
    global fails
    if sys.argv[1] == "make-wavs":
        make_wavs(sys.argv[2])
        return 0
    scenario, d, audioctl = sys.argv[1], sys.argv[2], sys.argv[3]
    slow = 3 if scenario == "call-slow" else 1      # qemu needs longer to settle
    alice, bob = Side(d, "alice", 4441, audioctl), Side(d, "bob", 4442, audioctl)

    for s in (alice, bob):
        check("OK" in s.sip.cmd("reginfo")["data"], f"{s.name} registered")
        s.audio("volume", "handset", "15", "volume", "speaker", "15", "volume", "ringer", "15")

    # alice lifts the handset, hears dial tone, dials, hears ringback
    alice.audio("route", "handset", "tone", "dial")
    time.sleep(1.0 * slow)
    a_dialled = alice.mark()
    alice.audio("tone", "off")
    alice.sip.cmd("dial", "sip:bob@127.0.0.1")
    if not bob.sip.wait_event("CALL_INCOMING", 10 * slow):
        print("FAIL bob saw no incoming call")
        return 1
    alice.audio("tone", "ringback")
    bob.audio("ring", "1")
    time.sleep(1.2 * slow)
    # bob answers: ringer off first, then the route, then tell baresip
    b_answered = bob.mark()
    a_answered = alice.mark()
    bob.audio("ring", "0")
    bob.audio("route", "speaker" if scenario == "echo-speaker" else "handset")
    alice.audio("tone", "off")
    bob.sip.cmd("accept")
    if not (alice.sip.wait_event("CALL_ESTABLISHED", 10 * slow) and bob.sip.wait_event("CALL_ESTABLISHED", 10 * slow)):
        print("FAIL call not established")
        return 1
    check("call=1" in alice.audio() and "call=1" in bob.audio(), "both audio services have a call stream")
    time.sleep(1.0 * slow)                       # jitter buffers settle
    a_talk0, b_talk0 = alice.mark(), bob.mark()
    time.sleep(3.0)
    a_talk1, b_talk1 = alice.mark(), bob.mark()

    if scenario in ("call", "call-slow"):
        alice.audio("mute", "1")
        time.sleep(0.8 * slow)
        b_mute0 = bob.mark()
        time.sleep(1.5)
        b_mute1 = bob.mark()
        alice.audio("mute", "0")
        time.sleep(0.5)

    alice.sip.cmd("hangup")
    check(bool(bob.sip.wait_event("CALL_CLOSED", 10 * slow)), "bob saw the call close")
    for s in (alice, bob):
        s.audio("route", "none")
    time.sleep(0.6 * slow)
    check("call=0" in alice.audio() and "idle" in alice.audio(), "alice's audio service is idle after the call")
    check("call=0" in bob.audio() and "idle" in bob.audio(), "bob's audio service is idle after the call")

    ap, bp = alice.played(), bob.played()       # complete: the service went idle

    # before the call
    check(best(ap[:a_dialled], 350) > 4000 and best(ap[:a_dialled], 440) > 4000, "alice heard dial tone before dialling")
    check(best(ap[a_dialled:a_answered], 400) > 3000 and best(ap[a_dialled:a_answered], 450) > 3000, "alice heard ringback while bob rang")
    check(best(bp[:b_answered], 1000, 320) > 12000, "bob's ringer rang (first note 1000 Hz)")
    check(best(bp[b_answered + 4800:], 1250, 320) < 2000, "bob's ringer stopped when he answered")

    a_talk, b_talk = ap[a_talk0:a_talk1], bp[b_talk0:b_talk1]
    if scenario in ("call", "call-slow"):
        a, b = worst(b_talk, ALICE_HZ), worst(a_talk, BOB_HZ)
        check(a > LEVEL * 0.6, "bob heard alice's file all through the talk (700 Hz at %.0f of %d)" % (a, LEVEL))
        check(b > LEVEL * 0.6, "alice heard bob's file all through the talk (1100 Hz at %.0f of %d)" % (b, LEVEL))
        check(best(b_talk, BOB_HZ) < 100 and best(a_talk, ALICE_HZ) < 100, "neither heard their own microphone")
        m = best(bp[b_mute0:b_mute1], ALICE_HZ)
        check(m < 30, "while alice was muted bob heard nothing of her (%.0f)" % m)
    else:
        # bob's microphone hears only the room: alice's voice coming back
        sent = worst(b_talk, ALICE_HZ)
        back = best(a_talk[len(a_talk) // 3:], ALICE_HZ)
        db = 20 * math.log10(max(back, 0.01) / max(sent, 0.01))
        print("  bob's loudspeaker played alice at %.0f; alice heard herself back at %.0f (%.1f dB)" % (sent, back, db))
        check(sent > LEVEL * 0.6, "bob's loudspeaker played alice")
        if scenario == "echo-handset":
            check(-9 < db < -3, "no echo control on this route: the room's echo (half level) comes straight back")
        else:
            check(db < -30, "speakerphone: the echo is cut by more than 30 dB")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
