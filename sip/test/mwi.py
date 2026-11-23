#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A voicemail server's message-waiting side, for sip/test/phone-test.sh: it
answers SUBSCRIBE for message-summary (RFC 3842) and sends NOTIFY saying how
many messages wait. The test registrar passes every SUBSCRIBE here. Nothing
is stored and no mail exists: the counts are whatever the test says.

  mwi.py serve DIR [PORT]        answer on 127.0.0.1:PORT (5075); the count for a user is
                                 the file DIR/USER, "NEW [OLD]", read again when it changes
  mwi.py set DIR USER NEW [OLD]  write that file: the server notices within a quarter second
"""
import os, random, re, socket, sys, time


def headers(msg):
    """First line, a dict of lower-case header name to list of values, and the body."""
    head, _, body = msg.partition("\r\n\r\n")
    lines = head.split("\r\n")
    h = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        h.setdefault(name.strip().lower(), []).append(value.strip())
    return lines[0], h, body


def count(d, user):
    try:
        f = open(os.path.join(d, user)).read().split()
        return int(f[0]), int(f[1]) if len(f) > 1 else 0
    except (OSError, ValueError, IndexError):
        return 0, 0


def serve(d, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(0.25)
    subs = {}      # call-id -> subscription
    seen = {}      # user -> the counts last sent
    os.makedirs(d, exist_ok=True)

    def notify(sub):
        new, old = count(d, sub["user"])
        sub["cseq"] += 1
        body = ("Messages-Waiting: %s\r\nMessage-Account: sip:%s@127.0.0.1\r\nVoice-Message: %d/%d (0/0)\r\n"
                % ("yes" if new else "no", sub["user"], new, old))
        msg = ("NOTIFY %s SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 127.0.0.1:%d;branch=z9hG4bK%08x\r\n"
               "Max-Forwards: 70\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %d NOTIFY\r\n"
               "Contact: <sip:mwi@127.0.0.1:%d>\r\nEvent: message-summary\r\n"
               "Subscription-State: active;expires=600\r\n"
               "Content-Type: application/simple-message-summary\r\nContent-Length: %d\r\n\r\n%s"
               % (sub["target"], port, random.getrandbits(32), sub["local"], sub["remote"], sub["callid"],
                  sub["cseq"], port, len(body), body))
        s.sendto(msg.encode(), sub["addr"])
        print("mwi: NOTIFY %s: %d new" % (sub["user"], new), flush=True)

    while True:
        try:
            data, src = s.recvfrom(8192)
        except socket.timeout:
            data = None
        if data:
            first, h, _ = headers(data.decode(errors="replace"))
            if first.startswith("SUBSCRIBE "):
                to, frm, callid = h["to"][0], h["from"][0], h["call-id"][0]
                m = re.search(r"sips?:([^@;>]+)@", to)
                c = re.search(r"<([^>]+)>", h.get("contact", [""])[0])
                if "tag=" not in to:
                    to += ";tag=%08x" % random.getrandbits(32)
                expires = int(h.get("expires", ["600"])[0])
                resp = "SIP/2.0 200 OK\r\n" + "".join("Via: %s\r\n" % v for v in h["via"]) + (
                    "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\nContact: <sip:mwi@127.0.0.1:%d>\r\n"
                    "Expires: %d\r\nContent-Length: 0\r\n\r\n" % (frm, to, callid, h["cseq"][0], port, expires))
                s.sendto(resp.encode(), src)
                if not m or not c:
                    continue
                if not expires:
                    subs.pop(callid, None)
                    continue
                hp = re.search(r"@([^:;>]+)(?::(\d+))?", c.group(1))
                sub = subs.get(callid) or {"cseq": 0}
                sub.update(user=m.group(1), target=c.group(1), local=to, remote=frm, callid=callid,
                           addr=(hp.group(1), int(hp.group(2) or 5060)))
                subs[callid] = sub
                print("mwi: SUBSCRIBE from %s" % sub["user"], flush=True)
                time.sleep(0.05)       # the 200 first
                notify(sub)
                seen[sub["user"]] = count(d, sub["user"])
        for sub in list(subs.values()):
            now = count(d, sub["user"])
            if seen.get(sub["user"]) != now:
                for other in subs.values():
                    if other["user"] == sub["user"]:
                        notify(other)
                seen[sub["user"]] = now


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "serve":
        serve(a[1], int(a[2]) if len(a) > 2 else 5075)
    elif len(a) >= 4 and a[0] == "set":
        os.makedirs(a[1], exist_ok=True)
        tmp = os.path.join(a[1], "." + a[2])
        with open(tmp, "w") as f:
            f.write("%d %d\n" % (int(a[3]), int(a[4]) if len(a) > 4 else 0))
        os.rename(tmp, os.path.join(a[1], a[2]))
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
