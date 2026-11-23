#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The far end of a test call, for sip/test/phone-test.sh: one-line commands
to a baresip over its t48ctrl socket, small enough to be called from a
simulator script's `run` lines.

  far.py SOCKET cmd COMMAND [PARAMS]     send one command; exit 0 if baresip said ok
  far.py SOCKET waitcall STATE [SECS]    wait until it has a call in STATE (incoming, outgoing,
                                         ringing, established) or, for STATE none, no call at all
  far.py SOCKET accept                   wait for an incoming call, then answer it
  far.py SOCKET log FILE                 write every event to FILE, one JSON object a line, until killed
  far.py waitlog FILE REGEX [SECS]       wait until a line of FILE matches
"""
import json, os, re, socket, sys, time


class Ctrl:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.buf = b""
        self.events = []

    def recv_one(self, timeout):
        end = time.time() + timeout
        while True:
            if b":" in self.buf:
                n, rest = self.buf.split(b":", 1)
                if len(rest) > int(n):
                    msg, self.buf = rest[:int(n)], rest[int(n) + 1:]
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
                raise SystemExit("far.py: baresip closed the socket")
            self.buf += d

    def cmd(self, command, params=None, timeout=5):
        m = {"command": command, "token": "t"}
        if params:
            m["params"] = params
        b = json.dumps(m).encode()
        self.s.sendall(b"%d:%s," % (len(b), b))
        end = time.time() + timeout
        while time.time() < end:
            r = self.recv_one(end - time.time())
            if r and r.get("response"):
                return r
            if r:
                self.events.append(r)
        raise SystemExit(f"far.py: no answer to {command}")

    def calls(self):
        """States of the calls baresip has now, from the events `sync` brings."""
        self.events = []
        self.cmd("sync")
        while True:
            e = self.recv_one(0.15)
            if not e:
                break
            self.events.append(e)
        return [e["type"][5:].lower() for e in self.events if e.get("sync") and e.get("class") == "call"]


def waitcall(c, state, secs):
    end = time.time() + secs
    while True:
        calls = c.calls()
        if (state == "none" and not calls) or state in calls:
            return True
        if time.time() > end:
            print(f"far.py: wanted a call in state {state}, have {calls or 'none'}", file=sys.stderr)
            return False
        time.sleep(0.1)


def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "waitlog":
        end = time.time() + (float(a[3]) if len(a) > 3 else 5)
        while True:
            if os.path.exists(a[1]) and any(re.search(a[2], line) for line in open(a[1])):
                return 0
            if time.time() > end:
                print(f"far.py: nothing in {a[1]} matches {a[2]}", file=sys.stderr)
                return 1
            time.sleep(0.1)
    if len(a) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    c = Ctrl(a[0])
    if a[1] == "cmd":
        r = c.cmd(a[2], " ".join(a[3:]) or None)
        if not r["ok"]:
            print(f"far.py: {a[2]}: {r['data'].strip()}", file=sys.stderr)
        return 0 if r["ok"] else 1
    if a[1] == "waitcall":
        return 0 if waitcall(c, a[2], float(a[3]) if len(a) > 3 else 5) else 1
    if a[1] == "accept":
        if not waitcall(c, "incoming", 5):
            return 1
        return 0 if c.cmd("accept")["ok"] else 1
    if a[1] == "log":
        with open(a[2], "a", buffering=1) as f:
            while True:
                e = c.recv_one(3600)
                if e and e.get("event"):
                    f.write(json.dumps(e) + "\n")
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
