#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Drive two baresip instances over t48ctrl (netstring-framed JSON on a Unix
socket in each one's run directory) for
sip/test/call-test.sh: check both registered, alice dials bob, bob answers,
both hear the other's tone, alice hangs up. Exit 0 when every check passed."""
import json, math, re, socket, struct, sys, time, wave


class Ctrl:
    def __init__(self, name, path):
        self.name = name
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(1)
        self.s.connect(path)
        self.buf = b""
        self.tok = 0

    def _recv_one(self, timeout):
        end = time.time() + timeout
        while True:
            if b":" in self.buf:
                n, rest = self.buf.split(b":", 1)
                n = int(n)
                if len(rest) > n:
                    msg, self.buf = rest[:n], rest[n + 1:]
                    return json.loads(msg)
            left = end - time.time()
            if left <= 0:
                return None
            self.s.settimeout(left)
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                return None
            if not d:
                raise SystemExit(f"{self.name}: control socket closed")
            self.buf += d

    def cmd(self, command, params=None, timeout=5):
        self.tok += 1
        m = {"command": command, "token": str(self.tok)}
        if params is not None:
            m["params"] = params
        b = json.dumps(m).encode()
        self.s.sendall(b"%d:%s," % (len(b), b))
        end = time.time() + timeout
        while time.time() < end:
            r = self._recv_one(end - time.time())
            if r and r.get("response") and r.get("token") == str(self.tok):
                return r
            if r and r.get("event"):
                self.events.append(r)
        raise SystemExit(f"{self.name}: no answer to {command}")

    events = None

    def wait_event(self, etype, timeout=10):
        end = time.time() + timeout
        for i, e in enumerate(self.events):
            if e.get("type") == etype:
                return self.events.pop(i)
        while time.time() < end:
            e = self._recv_one(end - time.time())
            if e and e.get("event"):
                print(f"  {self.name} event {e.get('type')} {e.get('param', '')}".rstrip())
                if e.get("type") == etype:
                    return e
                self.events.append(e)
        return None


def tone_hz(path):
    """Strongest of a few candidate frequencies, by Goertzel, plus RMS."""
    with wave.open(path) as w:
        rate, n = w.getframerate(), w.getnframes()
        data = w.readframes(n)
    s = struct.unpack("<%dh" % (len(data) // 2), data)
    s = s[len(s) // 4:]                     # skip call set-up
    if not s:
        return 0, 0.0, rate
    rms = math.sqrt(sum(x * x for x in s) / len(s))
    best, bestp = 0, 0.0
    for f in (440, 1000):
        k = 2 * math.cos(2 * math.pi * f / rate)
        a = b = 0.0
        for x in s:
            a, b = x + k * a - b, a
        p = a * a + b * b - k * a * b
        if p > bestp:
            best, bestp = f, p
    return best, rms, rate


def main():
    # what each side should hear: a frequency, "silence", or "none" (no file)
    expect_bob_hears = sys.argv[1] if len(sys.argv) > 1 else "440"
    expect_alice_hears = sys.argv[2] if len(sys.argv) > 2 else "1000"
    wavdir = sys.argv[3] if len(sys.argv) > 3 else "."
    ok = True
    alice, bob = Ctrl("alice", f"{wavdir}/alice/run/ctrl.sock"), Ctrl("bob", f"{wavdir}/bob/run/ctrl.sock")
    alice.events, bob.events = [], []

    for c in (alice, bob):
        r = c.cmd("reginfo")
        info = " ".join(re.sub(r"\x1b\[[0-9;]*m", "", r["data"]).split())
        print(f"  {c.name} reginfo: {info}")
        if "OK" not in r["data"]:
            print(f"FAIL {c.name} not registered")
            ok = False

    r = alice.cmd("dial", "sip:bob@127.0.0.1")
    print(f"  alice dial: ok={r['ok']}")
    if not bob.wait_event("CALL_INCOMING", 10):
        print("FAIL bob saw no incoming call")
        return 1
    r = bob.cmd("accept")
    print(f"  bob accept: ok={r['ok']}")
    ea = alice.wait_event("CALL_ESTABLISHED", 10)
    eb = bob.wait_event("CALL_ESTABLISHED", 10)
    if not (ea and eb):
        print("FAIL call not established")
        return 1
    print("  call established; talking for 4 s")
    time.sleep(4)
    r = alice.cmd("hangup")
    print(f"  alice hangup: ok={r['ok']}")
    if not bob.wait_event("CALL_CLOSED", 10):
        print("FAIL bob did not see the call close")
        ok = False
    alice.wait_event("CALL_CLOSED", 3)
    time.sleep(0.5)

    for who, path, want in (("bob", f"{wavdir}/bob-heard.wav", expect_bob_hears),
                            ("alice", f"{wavdir}/alice-heard.wav", expect_alice_hears)):
        if want == "none":
            continue
        f, rms, rate = tone_hz(path)
        if want == "silence":
            good = rms < 50
            print(f"  {who} heard rms {rms:.0f} at {rate} Hz: want silence: {'ok' if good else 'FAIL'}")
        else:
            good = f == int(want) and rms > 1000
            print(f"  {who} heard {f} Hz, rms {rms:.0f}, file at {rate} Hz: want {want} Hz: {'ok' if good else 'FAIL'}")
        ok = ok and good
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
