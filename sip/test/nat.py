#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A home router's UDP NAT, on loopback, for sip/test/nat-test.sh.

    nat.py LISTEN_PORT SERVER_PORT IDLE_SECONDS

The phone sends to 127.0.0.1:LISTEN_PORT as if it were the server. Each
phone address gets its own outside socket (a binding) on a fresh port, and
what it sends goes to the server at 127.0.0.1:SERVER_PORT from there. What
arrives at the binding goes back to the phone. A binding that has sent
nothing out for IDLE_SECONDS is closed, as routers do; packets for it are
then lost (the server gets "port unreachable"). SIGUSR1 drops every binding
at once, like a router restarting.

Prints one line per binding made or dropped and per SIP request passed out,
with the time since start."""
import select, signal, socket, sys, time

listen_port, server_port, idle = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
server = ("127.0.0.1", server_port)
t0 = time.time()


def log(msg):
    print(f"{time.time() - t0:7.1f} {msg}", flush=True)


inside = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
inside.bind(("127.0.0.1", listen_port))
bindings = {}     # phone address -> [outside socket, time of last packet out]
drop_all = False


def on_usr1(*_):
    global drop_all
    drop_all = True


signal.signal(signal.SIGUSR1, on_usr1)


def drop(phone, why):
    s, _ = bindings.pop(phone)
    log(f"binding {phone[1]} -> {s.getsockname()[1]} dropped ({why})")
    s.close()


while True:
    if drop_all:
        drop_all = False
        for p in list(bindings):
            drop(p, "router restarted")
    now = time.time()
    for p, (s, last) in list(bindings.items()):
        if now - last > idle:
            drop(p, f"idle {now - last:.0f} s")
    socks = [inside] + [b[0] for b in bindings.values()]
    try:
        ready, _, _ = select.select(socks, [], [], 0.5)
    except InterruptedError:
        continue
    for s in ready:
        try:
            data, src = s.recvfrom(65535)
        except OSError:
            continue
        if s is inside:
            b = bindings.get(src)
            if not b:
                o = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                o.bind(("127.0.0.1", 0))
                b = bindings[src] = [o, 0]
                log(f"binding {src[1]} -> {o.getsockname()[1]} made")
            b[1] = time.time()
            first = data.split(b"\r\n", 1)[0].decode(errors="replace")
            if not first.startswith("SIP/2.0"):
                log(f"out {src[1]} -> {b[0].getsockname()[1]}: {first}")
            b[0].sendto(data, server)
        else:
            for p, (o, _) in bindings.items():
                if o is s:
                    inside.sendto(data, p)
                    break
