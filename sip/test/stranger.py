#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A caller that is not the phone's server, for sip/test/phone-test.sh (issue
#77, Block direct calls): one INVITE over UDP straight to the phone, from an
address of our choosing, and the answer it gets.

  stranger.py FROM_IP PHONE_IP:PORT USER WANT

WANT is a status code. For a final one (403) it must be the final answer.
For 180 the phone must ring; the script then waits up to 20 s for the call
to be turned away (the test presses X), acknowledges that, and is done.
Exit 0 if it went as wanted.
"""
import random, socket, sys, time

src, dst, user, want = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
host, port = dst.rsplit(":", 1)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind((src, 0))
me = "%s:%d" % s.getsockname()
tag, branch, callid = "%08x" % random.getrandbits(32), "z9hG4bK%08x" % random.getrandbits(32), "%016x@stranger" % random.getrandbits(64)
sdp = ("v=0\r\no=- 1 1 IN IP4 %s\r\ns=-\r\nc=IN IP4 %s\r\nt=0 0\r\n"
       "m=audio 40000 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n") % (src, src)


def request(method, cseq_method, extra=""):
    return ("%s sip:%s@%s SIP/2.0\r\nVia: SIP/2.0/UDP %s;branch=%s;rport\r\nMax-Forwards: 70\r\n"
            "From: <sip:mallory@%s>;tag=%s\r\nTo: <sip:%s@%s>%s\r\nCall-ID: %s\r\nCSeq: 1 %s\r\n"
            "Contact: <sip:mallory@%s>\r\n" % (method, user, dst, me, branch, src, tag, user, dst, extra, callid, cseq_method, me))


invite = request("INVITE", "INVITE") + "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s" % (len(sdp), sdp)
s.sendto(invite.encode(), (host, int(port)))
s.settimeout(1.0)
end, codes, rang = time.time() + (25 if want == 180 else 5), [], False
while time.time() < end:
    try:
        data, _ = s.recvfrom(65536)
    except socket.timeout:
        if not codes:
            s.sendto(invite.encode(), (host, int(port)))   # UDP: send again, as a real caller would
        continue
    line = data.split(b"\r\n", 1)[0].decode()
    code = int(line.split()[1])
    codes.append(code)
    rang |= code == 180
    if code >= 200:
        to = next(l for l in data.decode().split("\r\n") if l.lower().startswith("to:"))
        ack = request("ACK", "ACK").replace("To: <sip:%s@%s>\r\n" % (user, dst), to + "\r\n") + "Content-Length: 0\r\n\r\n"
        s.sendto(ack.encode(), (host, int(port)))
        break
print("stranger.py: %s from %s: %s" % (dst, src, " ".join(map(str, codes)) or "no answer"))
ok = rang if want == 180 else bool(codes) and codes[-1] == want
sys.exit(0 if ok else 1)
