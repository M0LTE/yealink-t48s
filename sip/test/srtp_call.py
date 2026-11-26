#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""One call for sip/test/tls-test.sh (issue #78): alice dials bob by number,
as the shell does, bob answers, and the media is checked: encrypted or not,
and each side hears the other's tone.

    srtp_call.py DIR secure|plain|refused

secure: both sides report CALL_MENC (SRTP up) and say "secure" when asked
to catch up (sync), and the tones get through. plain: no CALL_MENC, tones get
through. refused: the call is never established. Exit 0 when it went so."""
import sys, time
from ctrl import Ctrl, tone_hz


def secure_in_sync(c):
    """The call's "secure" flag from a fresh sync, as a restarted shell sees it."""
    c.cmd("sync")
    end = time.time() + 3
    while time.time() < end:
        e = c._recv_one(end - time.time())
        if e and e.get("sync") and e.get("class") == "call":
            return e.get("secure")
    return None


def main():
    w, want = sys.argv[1], sys.argv[2]
    alice, bob = Ctrl("alice", w + "/alice/run/ctrl.sock"), Ctrl("bob", w + "/bob/run/ctrl.sock")
    alice.events, bob.events = [], []
    for c in (alice, bob):
        if "OK" not in c.cmd("reginfo")["data"]:
            print(f"  {c.name} not registered")
            return 1
    alice.cmd("dial", "bob")
    if not bob.wait_event("CALL_INCOMING", 10):
        if want == "refused":
            print("  bob never rang: refused before ringing")
            return 0
        print("  bob saw no incoming call")
        return 1
    bob.cmd("accept")
    ea, eb = alice.wait_event("CALL_ESTABLISHED", 8), bob.wait_event("CALL_ESTABLISHED", 3)
    if want == "refused":
        alice.cmd("hangup")
        bob.cmd("hangup")
        print("  established, but should not have been" if ea and eb else "  not established: refused")
        return 1 if ea and eb else 0
    if not (ea and eb):
        print("  call not established")
        return 1
    ma, mb = alice.wait_event("CALL_MENC", 3), bob.wait_event("CALL_MENC", 1)
    for who, m in (("alice", ma), ("bob", mb)):
        print(f"  {who}: {'CALL_MENC ' + m.get('param', '') if m else 'no CALL_MENC'}")
    sa, sb = secure_in_sync(alice), secure_in_sync(bob)
    print(f"  sync says secure: alice {sa}, bob {sb}")
    time.sleep(4)
    alice.cmd("hangup")
    bob.wait_event("CALL_CLOSED", 5)
    time.sleep(0.5)
    ok = (bool(ma and mb) and sa is True and sb is True) if want == "secure" else (not ma and not mb and sa is False and sb is False)
    for who, path, f0 in (("bob", w + "/bob-heard.wav", 440), ("alice", w + "/alice-heard.wav", 1000)):
        f, rms, _ = tone_hz(path)
        good = f == f0 and rms > 1000
        print(f"  {who} heard {f} Hz, rms {rms:.0f}: {'ok' if good else 'FAIL'}")
        ok = ok and good
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
