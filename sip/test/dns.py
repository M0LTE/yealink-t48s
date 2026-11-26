#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A DNS server for the SIP over TLS test (sip/test/tls-test.sh, issue #78):
UDP on loopback, answering only the names it is given, so baresip can find a
test server by SRV and check its certificate against a name.

    dns.py PORT RECORD...
      A:name=address               an A record
      SRV:name=prio,weight,port,target

Any other name gets NXDOMAIN; a known name asked for another type gets an
empty answer."""
import socket, struct, sys


def name_bytes(name):
    return b"".join(bytes([len(p)]) + p.encode() for p in name.rstrip(".").split(".")) + b"\0"


def read_name(msg, at):
    labels = []
    while True:
        n = msg[at]
        if n == 0:
            return ".".join(labels).lower(), at + 1
        if n & 0xC0:                       # a pointer; questions never have one
            raise ValueError("compressed name")
        labels.append(msg[at + 1:at + 1 + n].decode("ascii", "replace"))
        at += 1 + n


def main():
    port = int(sys.argv[1])
    a, srv = {}, {}
    for r in sys.argv[2:]:
        kind, rest = r.split(":", 1)
        name, value = rest.split("=", 1)
        if kind == "A":
            a.setdefault(name.lower(), []).append(value)
        elif kind == "SRV":
            prio, weight, p, target = value.split(",")
            srv.setdefault(name.lower(), []).append((int(prio), int(weight), int(p), target))
    known = set(a) | set(srv)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    while True:
        msg, peer = s.recvfrom(1500)
        try:
            qid, flags, qd = struct.unpack(">HHH", msg[:6])
            name, at = read_name(msg, 12)
            qtype, qclass = struct.unpack(">HH", msg[at:at + 4])
        except (ValueError, struct.error, IndexError):
            continue
        question = msg[12:at + 4]
        answers = []
        if qtype == 1:
            for ip in a.get(name, []):
                answers.append(struct.pack(">HHIH", 1, 1, 60, 4) + socket.inet_aton(ip))
        elif qtype == 33:
            for prio, weight, p, target in srv.get(name, []):
                rdata = struct.pack(">HHH", prio, weight, p) + name_bytes(target)
                answers.append(struct.pack(">HHIH", 33, 1, 60, len(rdata)) + rdata)
        rcode = 0 if name in known else 3
        head = struct.pack(">HHHHHH", qid, 0x8400 | (flags & 0x0100) | rcode, 1, len(answers), 0, 0)
        # each answer names the question's name by pointer (offset 12)
        s.sendto(head + question + b"".join(b"\xc0\x0c" + x for x in answers), peer)


if __name__ == "__main__":
    main()
