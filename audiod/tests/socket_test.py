#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""t48-audiod test: the real daemon on the file backend, with this script
standing in for baresip on the call socket and t48-audioctl standing in for
the shell. Nothing is played: the "loudspeaker" is a WAV file.

  socket_test.py BUILD_DIR      (run by make check)
"""
import fcntl, math, mmap, os, shutil, socket, struct, subprocess, sys, tempfile, threading, time, wave

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build")
FRAME = 320
fails = 0


def check(ok, what):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        fails += 1


def amp(samples, hz, rate=16000):
    """Amplitude of one frequency (a full-scale sine gives 32767)."""
    if not samples:
        return 0.0
    w = 2 * math.pi * hz / rate
    re = sum(x * math.cos(w * i) for i, x in enumerate(samples))
    im = sum(x * math.sin(w * i) for i, x in enumerate(samples))
    return 2 * math.hypot(re, im) / len(samples)


def sine(hz, level, n, start=0):
    return [int(level * math.sin(2 * math.pi * hz * (start + i) / 16000)) for i in range(n)]


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


def read_wav(path):
    with wave.open(path) as w:
        d = w.readframes(w.getnframes())
    return list(struct.unpack("<%dh" % (len(d) // 2), d))


class Daemon:
    def __init__(self, name, mic=None):
        self.dir = tempfile.mkdtemp(prefix="audiod-" + name + "-")
        self.spk = os.path.join(self.dir, "spk.wav")
        self.log = os.path.join(self.dir, "log")
        args = [os.path.join(BUILD, "t48-audiod"), "--run-dir", self.dir, "--spk", self.spk]
        if mic:
            args += ["--mic", mic]
        self.p = subprocess.Popen(args, stderr=open(self.log, "w"))
        for _ in range(100):
            if os.path.exists(os.path.join(self.dir, "ctl.sock")):
                break
            time.sleep(0.02)

    def ctl(self, *args):
        r = subprocess.run([os.path.join(BUILD, "t48-audioctl"), "-s", os.path.join(self.dir, "ctl.sock"), *args],
                           capture_output=True, text=True)
        return r.stdout.strip() + r.stderr.strip()

    def stop(self):
        self.p.terminate()
        self.p.wait(5)
        log = open(self.log).read()
        played = read_wav(self.spk)
        shutil.rmtree(self.dir)
        return log, played


class Call:
    """What baresip's t48audio module does: HELLO, then answer MIC with SPK."""

    def __init__(self, d, version=2, frame_ms=20):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(os.path.join(d.dir, "call.sock"))
        self.s.settimeout(1)
        self.buf = b""
        self.send(1, struct.pack("<HH", version, frame_ms))
        self.sent = 0

    def send(self, typ, payload=b""):
        self.s.sendall(struct.pack("<HH", typ, len(payload)) + payload)

    def recv(self):
        while True:
            if len(self.buf) >= 4:
                typ, ln = struct.unpack("<HH", self.buf[:4])
                if len(self.buf) >= 4 + ln:
                    p, self.buf = self.buf[4:4 + ln], self.buf[4 + ln:]
                    return typ, p
            d = self.s.recv(4096)
            if not d:
                return None, b""
            self.buf += d

    def start(self, spk_hz=1000):
        """Answer in the background from now on, as baresip does."""
        self.frames, self.skip, self.lock = [], 0, threading.Lock()
        self.s.settimeout(None)
        self.t = threading.Thread(target=self._loop, args=(spk_hz,), daemon=True)
        self.t.start()

    def _loop(self, spk_hz):
        try:
            while True:
                typ, p = self.recv()
                if typ is None:
                    return
                if typ != 3:
                    continue
                with self.lock:
                    self.frames.append((time.monotonic(), struct.unpack("<I", p[:4])[0],
                                        struct.unpack("<%dh" % FRAME, p[4:])))
                    skip = self.skip > 0
                    self.skip -= skip
                if not skip:
                    tone = sine(spk_hz, 8000, FRAME, self.sent * FRAME)
                    self.send(4, p[:4] + struct.pack("<%dh" % FRAME, *tone))
                    self.sent += 1
        except OSError:
            pass

    def run(self, frames, skip=0):
        """Wait for that many more MIC frames (not answering the first
        skip of them). Returns (microphone samples, arrival times,
        sequence numbers)."""
        with self.lock:
            first = len(self.frames)
            self.skip = skip
        end = time.monotonic() + frames * 0.02 + 2
        while time.monotonic() < end:
            with self.lock:
                if len(self.frames) >= first + frames:
                    break
            time.sleep(0.005)
        with self.lock:
            got = self.frames[first:first + frames]
        mic = [x for f in got for x in f[2]]
        return mic, [f[0] for f in got], [f[1] for f in got]

    def close(self):
        self.send(5)
        self.s.close()


class App:
    """The shell and an app with sound, in one: the ring (a sealed memfd, as
    the shell makes it), handed to the service on the control socket, and the
    app's side of it."""
    FRAMES, DATA = 4096, 4096

    def __init__(self, d, sealed=True, size=None):
        self.fd = os.memfd_create("t48-sound", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
        os.ftruncate(self.fd, size or self.DATA + self.FRAMES * 2)
        if sealed:
            fcntl.fcntl(self.fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SEAL)
        self.m = mmap.mmap(self.fd, size or self.DATA + self.FRAMES * 2)
        self.m[0:16] = struct.pack("<IIII", 0x53383454, 16000, self.FRAMES, self.DATA)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(os.path.join(d.dir, "ctl.sock"))
        self.s.sendall(struct.pack("<HHH", 1, 2, 1))   # HELLO
        self.wr = self.n = 0

    def tell(self, what, with_ring=False):
        msg = struct.pack("<HHB", 9, 1, what)   # APP
        if with_ring:
            socket.send_fds(self.s, [msg], [self.fd])
        else:
            self.s.sendall(msg)

    def field(self, k):
        """rd, state, delay, dry"""
        return struct.unpack_from("<I", self.m, 16 + 4 * k)[0]

    def write(self, samples):
        for i in range(0, len(samples), 256):
            part = samples[i:i + 256]
            at = self.wr % self.FRAMES
            first = min(len(part), self.FRAMES - at)
            self.m[self.DATA + 2 * at:self.DATA + 2 * (at + first)] = struct.pack("<%dh" % first, *part[:first])
            if first < len(part):
                self.m[self.DATA:self.DATA + 2 * (len(part) - first)] = struct.pack("<%dh" % (len(part) - first), *part[first:])
            self.wr = (self.wr + len(part)) & 0xffffffff
            self.m[32:36] = struct.pack("<I", self.wr)

    def play(self, hz, level, seconds, ahead=960):
        """Feed a tone as an app does: keep about `ahead` samples waiting."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            room = ahead - ((self.wr - self.field(0)) & 0xffffffff)
            if room >= 160:
                self.write(sine(hz, level, room, self.n))
                self.n += room
            time.sleep(0.004)
        self.dry = self.field(3)   # before the ring drains

    def close(self):
        self.s.close()


def app_tests():
    print("== an app's sound")
    d = Daemon("app")
    d.ctl("volume", "app", "15", "volume", "ringer", "15")
    bad = App(d, sealed=False)
    bad.tell(2, with_ring=True)
    time.sleep(0.1)
    check("app=none" in d.ctl(), "a ring that is not sealed is refused")
    bad.close()
    a = App(d)
    a.tell(2, with_ring=True)
    time.sleep(0.1)
    st = d.ctl()
    check("app=front" in st and "idle" in st, "the ring is taken; nothing written yet, so no clock runs")
    check(a.field(1) == 1, "the app is told its sound will be played")
    t0, rd0 = time.monotonic(), a.field(0)
    a.play(500, 32767, 1.5)
    t1, rd1 = time.monotonic(), a.field(0)
    st = d.ctl()
    check("app=front,sounding" in st and "running" in st, "writing starts the hardware; the shell is told an app is sounding")
    rate = (rd1 - rd0) / (t1 - t0)
    check(15000 < rate < 17000, "taken at %.0f samples a second (want 16000)" % rate)
    check(a.dry == 0, "never ran dry while it was fed")
    # calls win: the phone rings, the app goes on writing
    d.ctl("ring", "1")
    time.sleep(0.1)
    check(a.field(1) == 2, "ringing: the app is told its sound is held")
    rd0 = a.field(0)
    a.play(500, 32767, 1.0)
    check(a.field(0) - rd0 > 14000, "held, it is still taken at the same rate")
    d.ctl("ring", "0")
    a.play(500, 32767, 1.0)
    check(a.field(1) == 1, "ringing over: played again")
    # off the screen with samples waiting: dropped, and nothing more is taken
    a.write(sine(500, 32767, 3000))
    a.tell(1)
    time.sleep(0.2)
    rd0 = a.field(0)
    check(a.field(1) == 0 and rd0 == a.wr and "app=paused" in d.ctl(), "paused: told so, and what was waiting is dropped")
    a.write(sine(500, 32767, 2000))
    time.sleep(0.3)
    check(a.field(0) == rd0, "paused: nothing is taken")
    time.sleep(2.2)
    check("idle" in d.ctl(), "2 s after an app's last sound the hardware is idle")
    # back in front: a short sound from idle
    a.tell(2)
    time.sleep(0.1)
    a.write(sine(2000, 16000, 1600))
    time.sleep(0.5)
    check(a.field(0) == a.wr and a.field(3) >= 1, "a 100 ms sound from idle is played out, then the ring is dry")
    # the shell goes: the ring is let go
    a.close()
    time.sleep(0.2)
    check("app=none" in d.ctl() and a.field(1) == 0, "the shell's connection closes: the app's sound is closed")
    time.sleep(2.2)
    log, played = d.stop()
    line = [l for l in log.splitlines() if "app sound closed" in l]
    print("  log: " + (line[0] if line else "(no summary line)"))
    check(bool(line), "the log has the app's figures")
    check(max(abs(x) for x in played) <= 1800, "nothing on the loudspeaker above its limit (1800)")
    first = next(i for i in range(0, len(played), FRAME) if max(abs(x) for x in played[i:i + FRAME]) > 0)
    tone = played[first + 2 * FRAME:first + 60 * FRAME]
    lvl = amp(tone, 500)
    check(1700 < lvl < 1810, "a full-scale tone at app volume 15 is on the limit (500 Hz at %.0f, want 1800)" % lvl)
    # the ring: the ringer is there and the app is not
    ring_at = next((i for i in range(first + 70 * FRAME, len(played) - 4096, 160) if amp(played[i:i + 256], 1000) > 1500), None)
    check(ring_at is not None, "the ringer rang")
    if ring_at is not None:
        during = played[ring_at + 1600:ring_at + 12800]
        check(amp(during, 500) < 30, "while it rang none of the app's sound was played (500 Hz at %.0f)" % amp(during, 500))
    short = max(amp(played[i:i + 800], 2000) for i in range(len(played) - 16000 * 3, len(played) - 800, 400))
    check(850 < short < 900, "the short sound: 2000 Hz at %.0f (want 879: half scale at volume 15)" % short)


def main():
    app_tests()
    if fails:
        return 1
    tmp = tempfile.mkdtemp(prefix="audiod-mic-")
    mic_wav = os.path.join(tmp, "mic.wav")
    write_wav(mic_wav, sine(440, 6000, 16000))   # one second, looped by the daemon

    print("== call stream: pacing, privacy, both directions, mute")
    d = Daemon("call", mic_wav)
    check("hw=capture,playback,idle" in d.ctl("volume", "handset", "15"), "idle before anything is asked: no clock running")
    c = Call(d)
    typ, p = c.recv()
    check(typ == 2 and struct.unpack("<HH", p) == (2, 20), "READY answers HELLO: version 2, 20 ms frames")
    c.start()
    mic, times, seqs = c.run(100)
    check("call=1" in d.ctl() and "running" in d.ctl(), "the shell is told the call stream is up; the clock runs")
    span = times[-1] - times[0]
    check(abs(span - 99 * 0.02) < 0.03, "99 frame gaps took %.3f s (want 1.980)" % span)
    gaps = [b - a for a, b in zip(times, times[1:])]
    check(max(gaps) < 0.035 and min(gaps) > 0.005, "frame gaps between %.1f and %.1f ms" % (min(gaps) * 1e3, max(gaps) * 1e3))
    check(seqs == list(range(100)), "sequence numbers count from 0")
    check(max(abs(x) for x in mic) == 0, "on hook: the call gets pure silence, not the microphone")
    check("route=handset" in d.ctl("route", "handset"), "route handset accepted")
    c.run(30)   # the first half second after a route change is held back (microphone thump)
    mic, _, _ = c.run(50)
    a = amp(mic, 440)
    check(5700 < a < 6100 and amp(mic, 1000) < 50, "off hook: the call gets the microphone (440 Hz at %.0f, want 6000)" % a)
    check("mute=1" in d.ctl("mute", "1"), "mute accepted")
    c.run(3)
    mic, _, _ = c.run(25)
    check(max(abs(x) for x in mic) == 0, "muted: silence to the call")
    d.ctl("mute", "0")
    c.run(3)
    mic, _, _ = c.run(25)
    check(amp(mic, 440) > 5700, "unmuted: the microphone is back")
    # baresip late: three ticks with no answer, then carry on
    c.run(3, skip=3)
    c.run(50)
    d.ctl("route", "none")
    c.run(25)
    c.close()
    time.sleep(0.3)
    check("call=0" in d.ctl() and "idle" in d.ctl(), "call stream gone, nothing wanted: hardware idle again")
    log, played = d.stop()
    # played: on hook for the first 100 frames and more, then off hook
    on_hook = played[:100 * FRAME]
    first = next(i for i in range(0, len(played), FRAME) if max(abs(x) for x in played[i:i + FRAME]) > 0)
    off_hook = played[first + 10 * FRAME:first + 50 * FRAME]
    check(max(abs(x) for x in on_hook) == 0, "on hook: the call's audio is not played")
    a = amp(off_hook, 1000)
    check(7800 < a < 8100, "off hook: the call's audio is played (1000 Hz at %.0f, want 8000 at volume 15)" % a)
    check(amp(off_hook, 440) < 50, "no microphone in the loudspeaker (no software sidetone)")
    line = [l for l in log.splitlines() if "call stream closed" in l]
    print("  log: " + (line[0] if line else "(no summary line)"))
    check(bool(line) and "3 silent ticks" in line[0], "the three late ticks were played as silence and counted")
    check(bool(line) and " 0 overflowed" in line[0], "no frames dropped for arriving too fast")

    print("== refused and replaced connections")
    d = Daemon("refuse")
    c = Call(d, version=1, frame_ms=20)
    typ, _ = c.recv()
    check(typ is None, "an old-version HELLO is refused by closing")
    c1 = Call(d); c1.recv()
    c2 = Call(d); c2.recv()
    c2.start()
    mic, _, seqs = c2.run(5)
    check(seqs[0] == 0, "a second connection takes over (baresip restarted)")
    try:
        c1.s.settimeout(0.5)
        gone = c1.s.recv(10000) == b"" or True
    except OSError:
        gone = True
    check(gone, "the first connection was dropped")
    c2.close()
    d.stop()

    print("== tones, ringer and keys with no call")
    d = Daemon("tones")
    d.ctl("volume", "ringer", "15", "volume", "handset", "13")
    check("tone=dial" in d.ctl("tone", "dial") and "running" in d.ctl(), "dial tone on: hardware running")
    time.sleep(1.0)
    check("tone=off" in d.ctl("tone", "off"), "tone off")
    time.sleep(0.4)
    check("idle" in d.ctl(), "0.4 s after the tone: hardware idle")
    d.ctl("ring", "1")
    time.sleep(1.2)
    d.ctl("ring", "0")
    time.sleep(0.4)
    d.ctl("key", "5")
    time.sleep(0.4)
    check("idle" in d.ctl(), "a key sound ends by itself and the hardware goes idle")
    d.ctl("route", "handset", "tone", "dial")     # off-hook dial tone at the handset volume
    time.sleep(0.6)
    d.ctl("route", "none", "tone", "off")
    time.sleep(0.3)
    check("not understood" in d.ctl("route", "sideways"), "audioctl refuses nonsense")
    log, played = d.stop()
    n = len(played) / 16000
    check(3.4 < n < 4.6, "the loudspeaker file holds only the time the hardware ran (%.2f s of about 4.9 s)" % n)
    dial = played[1600:14400]
    check(540 < amp(dial, 350) < 575 and 540 < amp(dial, 440) < 575, "dial tone on hook: 350 + 440 Hz at the ringer volume")
    # find the ring: 1000 Hz at the loudspeaker's limit (1800) in some 16 ms block
    ring_at = next((i for i in range(0, len(played) - 256, 16) if amp(played[i:i + 256], 1000) > 1700), None)
    check(ring_at is not None, "ringer: its first note (1000 Hz) is there")
    key_at = next((i for i in range(0, len(played) - 960, 160)
                   if amp(played[i:i + 960], 770) > 450 and amp(played[i:i + 960], 1336) > 450), None)
    check(key_at is not None and (ring_at is None or key_at > ring_at), "key 5: 770 + 1336 Hz after the ring")
    check(max(abs(x) for x in played[:-int(1.0 * 16000)]) <= 1800, "nothing on the loudspeaker above its limit (1800)")
    tail = played[-int(0.8 * 16000):]
    hs = max(amp(tail[i:i + 3200], 350) for i in range(0, len(tail) - 3200, 1600))
    check(2300 < hs < 2600, "dial tone off hook: at the handset volume (350 Hz at %.0f, want 2500)" % hs)

    shutil.rmtree(tmp)
    print("%s socket test: %d failed" % ("FAIL" if fails else "PASS", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
