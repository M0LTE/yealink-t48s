#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A scripted far end for sip/test/interop-test.sh (issue #77, docs/sip.md):
plays a caller or a callee towards one baresip, with the headers a test asks
for, and says what came back. Session timers (RFC 4028) and reliable
provisional responses (RFC 3262) are its business; it sends PCMU and ignores
the audio.

  uac.py call TARGET PORT [OPTION...]   call TARGET (sip:user@host:port) from
                                        127.0.0.1:PORT and print one line per
                                        step: "got 180 rel", "got 200 ...",
                                        "refresh update 200", ...
  options:
    hdr=NAME:VALUE      add a header to the INVITE (repeatable)
    answer-after=CTRL   answer the call over t48ctrl at socket CTRL
    refresh=update|reinvite|reinvite-nosdp   refresh the session once, after
                        two seconds, with this request (repeatable)
    hold=SECONDS        stay in the call this long before BYE (default 3)
    expect-bye          wait for the far end to hang up instead (hold is the limit)
    no-update           leave UPDATE out of Allow

  uac.py answer PORT [OPTION...]        answer one INVITE on 127.0.0.1:PORT
    hdr=NAME:VALUE      add a header to the 200 (repeatable)
    rel100              send 183 reliably first (needs the caller's support)
    slow-update         answer UPDATE with 100 Trying, then 200 half a second later
    hold=SECONDS        how long to wait for refreshes and the BYE (default 10)
"""
import random, re, socket, sys, time

SDP = ("v=0\r\no=uac 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
       "m=audio %d RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n")


def parse(data):
    head, _, body = data.decode(errors="replace").partition("\r\n\r\n")
    lines = head.split("\r\n")
    h = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        name = name.strip().lower()
        name = {"v": "via", "f": "from", "t": "to", "i": "call-id", "m": "contact",
                "k": "supported", "x": "session-expires", "l": "content-length"}.get(name, name)
        h.setdefault(name, []).append(value.strip())
    return lines[0], h, body


def hdr(h, name):
    return ", ".join(h.get(name, []))


def has_tag(h, name, tag):
    return tag in [t.strip().lower() for t in hdr(h, name).split(",")]


class Peer:
    def __init__(self, port):
        self.port = port
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", port))
        self.cseq = 1
        self.tag = "%08x" % random.getrandbits(32)
        self.rtp = port + 1000
        self.slow_update = False

    def send(self, text, dest):
        self.s.sendto(text.encode(), dest)

    def recv(self, timeout):
        self.s.settimeout(max(timeout, 0.01))
        try:
            data, src = self.s.recvfrom(65535)
        except socket.timeout:
            return None
        return parse(data) + (src,)

    def reply(self, first, h, src, code, reason, extra="", body=""):
        to = hdr(h, "to")
        if code > 100 and "tag=" not in to:
            to += ";tag=" + self.tag
        text = ("SIP/2.0 %d %s\r\n" % (code, reason) + "".join("Via: %s\r\n" % v for v in h["via"])
                + "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n" % (hdr(h, "from"), to, hdr(h, "call-id"), hdr(h, "cseq"))
                + "Contact: <sip:uac@127.0.0.1:%d>\r\n" % self.port + extra)
        if body:
            text += "Content-Type: application/sdp\r\n"
        text += "Content-Length: %d\r\n\r\n%s" % (len(body), body)
        self.send(text, src)

    def answer_request(self, first, h, body, src):
        """Answer what the far end sends inside the dialog. Returns the method."""
        method = first.split()[0]
        if method == "ACK":
            return method
        if method == "UPDATE" and self.slow_update:
            self.reply(first, h, src, 100, "Trying")
            time.sleep(0.5)
        if method == "INVITE":
            self.reply(first, h, src, 200, "OK", body=SDP % self.rtp)
        else:
            self.reply(first, h, src, 200, "OK")
        se = hdr(h, "session-expires")
        print("got %s%s%s" % (method, " sdp" if body.strip() else "", (" se=" + se) if se else ""), flush=True)
        return method


def opts(args):
    o = {"hdr": [], "refresh": [], "hold": None, "answer-after": None, "expect-bye": False, "rel100": False, "no-update": False, "slow-update": False}
    for a in args:
        k, _, v = a.partition("=")
        if k in ("hdr", "refresh"):
            o[k].append(v)
        elif k in ("expect-bye", "rel100", "no-update", "slow-update"):
            o[k] = True
        else:
            o[k] = v
    return o


def call(target, port, o):
    p = Peer(port)
    m = re.match(r"sip:([^@]+)@([^:;>]+):(\d+)", target)
    dest = (m.group(2), int(m.group(3)))
    callid = "%08x@uac" % random.getrandbits(32)
    frm = "<sip:uac@127.0.0.1:%d>;tag=%s" % (port, p.tag)
    to = "<%s>" % target
    route_set, remote = [], target

    def request(method, uri, body="", extra="", cseq=None, branch=None):
        if cseq is None:
            p.cseq += 1
            cseq = p.cseq
        text = ("%s %s SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:%d;rport;branch=%s\r\n"
                "Max-Forwards: 70\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %d %s\r\n"
                "Contact: <sip:uac@127.0.0.1:%d>\r\n%s" % (method, uri, port,
                                                           branch or "z9hG4bK%08x" % random.getrandbits(32), frm, to,
                                                           callid, cseq, method, port, extra))
        if body:
            text += "Content-Type: application/sdp\r\n"
        text += "Content-Length: %d\r\n\r\n%s" % (len(body), body)
        p.send(text, dest)
        return cseq

    def transaction(method, body="", extra="", timeout=5):
        """Send a request in the dialog and wait for its final answer."""
        cseq = request(method, remote, body, extra)
        end = time.time() + timeout
        while time.time() < end:
            r = p.recv(end - time.time())
            if not r:
                break
            first, h, rbody, src = r
            if hdr(h, "call-id") != callid:
                continue
            if first.startswith("SIP/2.0") and hdr(h, "cseq").startswith("%d " % cseq):
                code = int(first.split()[1])
                if code >= 200:
                    return code, h, rbody
            elif not first.startswith("SIP/2.0"):
                p.answer_request(first, h, rbody, src)
        return None, {}, ""

    extra = "".join("%s\r\n" % x.replace(":", ": ", 1) for x in o["hdr"])
    extra += "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, %sPRACK\r\n" % ("" if o["no-update"] else "UPDATE, ")
    ibranch = "z9hG4bK%08x" % random.getrandbits(32)
    icseq = request("INVITE", target, SDP % p.rtp, extra, cseq=1, branch=ibranch)
    answered = False
    end = time.time() + 15
    final = None
    while time.time() < end:
        r = p.recv(0.2)
        if o["answer-after"] and not answered and time.time() > end - 13:
            sys.path.insert(0, sys.path[0])
            from ctrl import Ctrl
            c = Ctrl("callee", o["answer-after"])
            c.events = []
            if c.wait_event("CALL_INCOMING", 5):
                c.cmd("accept")
            answered = True
        if not r:
            continue
        first, h, body, src = r
        if hdr(h, "call-id") != callid:
            continue        # left over from an earlier call on this port
        if not first.startswith("SIP/2.0"):
            p.answer_request(first, h, body, src)
            continue
        code = int(first.split()[1])
        if not hdr(h, "cseq").endswith("INVITE"):
            continue
        if 100 < code < 200:
            rel = has_tag(h, "require", "100rel")
            print("got %d%s%s" % (code, " rel" if rel else "", " sdp" if body.strip() else ""), flush=True)
            if rel:
                if "tag=" in hdr(h, "to"):
                    to = hdr(h, "to")
                if h.get("contact"):
                    remote = re.search(r"<([^>]+)>", hdr(h, "contact")).group(1)
                route_set = list(reversed(h.get("record-route", [])))
                rseq = hdr(h, "rseq")
                code_p, _, _ = transaction("PRACK", extra="RAck: %s 1 INVITE\r\n" % rseq
                                           + "".join("Route: %s\r\n" % x for x in route_set))
                print("prack %s" % code_p, flush=True)
        elif code >= 200:
            final = (code, h, body)
            break
    if not final:
        print("no answer to INVITE", flush=True)
        return 1
    code, h, body = final
    print("got %d se=%s require=%s supported=%s" % (code, hdr(h, "session-expires") or "-",
                                                     hdr(h, "require") or "-", hdr(h, "supported") or "-"), flush=True)
    if code == 420 or code == 421:
        print("  unsupported=%s" % (hdr(h, "unsupported") or "-"), flush=True)
    if code >= 300:
        to = hdr(h, "to")
        request("ACK", target, cseq=icseq, branch=ibranch)
        return 0
    to = hdr(h, "to")
    remote = re.search(r"<([^>]+)>", hdr(h, "contact")).group(1)
    route_set = list(reversed(h.get("record-route", [])))
    routes = "".join("Route: %s\r\n" % x for x in route_set)
    request("ACK", remote, extra=routes, cseq=icseq)
    t0 = time.time()
    time.sleep(1)
    for kind in o["refresh"]:
        time.sleep(1)
        se = "".join("%s\r\n" % x.replace(":", ": ", 1) for x in o["hdr"] if x.lower().startswith(("session-expires", "supported")))
        if kind == "update":
            c, rh, _ = transaction("UPDATE", extra=routes + se)
        elif kind == "reinvite":
            c, rh, _ = transaction("INVITE", SDP % p.rtp, extra=routes + se)
            request("ACK", remote, extra=routes, cseq=p.cseq)
        else:
            c, rh, rb = transaction("INVITE", extra=routes + se)
            request("ACK", remote, SDP % p.rtp, extra=routes, cseq=p.cseq)
        print("refresh %s %s se=%s" % (kind, c, hdr(rh, "session-expires") or "-"), flush=True)
    hold = float(o["hold"] or 3)
    while time.time() - t0 < hold:
        r = p.recv(t0 + hold - time.time())
        if r and not r[0].startswith("SIP/2.0"):
            if p.answer_request(r[0], r[1], r[2], r[3]) == "BYE":
                print("far end hung up after %.0f s" % (time.time() - t0), flush=True)
                return 0
    if o["expect-bye"]:
        print("no BYE in %.0f s" % hold, flush=True)
        return 1
    c, _, _ = transaction("BYE", extra=routes)
    print("bye %s" % c, flush=True)
    return 0


def answer(port, o):
    p = Peer(port)
    p.slow_update = o["slow-update"]
    hold = float(o["hold"] or 10)
    end = None
    extra = "".join("%s\r\n" % x.replace(":", ": ", 1) for x in o["hdr"])
    while True:
        r = p.recv(1 if end is None else end - time.time())
        if end is not None and time.time() > end:
            print("no BYE in %.0f s" % hold, flush=True)
            return 1
        if not r:
            continue
        first, h, body, src = r
        if first.startswith("SIP/2.0"):
            continue
        method = first.split()[0]
        if method == "INVITE" and end is None:
            print("got INVITE se=%s supported=%s require=%s" % (hdr(h, "session-expires") or "-",
                                                             hdr(h, "supported") or "-", hdr(h, "require") or "-"), flush=True)
            if o["rel100"] and has_tag(h, "supported", "100rel"):
                p.reply(first, h, src, 183, "Session Progress", "Require: 100rel\r\nRSeq: 1\r\n", SDP % p.rtp)
                r2 = p.recv(3)
                if r2 and r2[0].startswith("PRACK"):
                    p.reply(r2[0], r2[1], r2[3], 200, "OK")
                    print("got PRACK rack=%s" % hdr(r2[1], "rack"), flush=True)
            body200 = "" if o["rel100"] else SDP % p.rtp
            p.reply(first, h, src, 200, "OK", extra + "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, UPDATE, PRACK\r\n", body200)
            end = time.time() + hold
            continue
        m = p.answer_request(first, h, body, src)
        if m == "BYE":
            print("got BYE", flush=True)
            return 0


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "call":
        return call(sys.argv[2], int(sys.argv[3]), opts(sys.argv[4:]))
    if len(sys.argv) >= 3 and sys.argv[1] == "answer":
        return answer(int(sys.argv[2]), opts(sys.argv[3:]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
