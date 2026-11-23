# SPDX-License-Identifier: AGPL-3.0-or-later
"""fakesip.py SOCKET: stands in for baresip's control socket (the t48ctrl module) so that
the shell's simulator can show an account as registered and a call as answered, for
pictures. Every account is registered; every call that is dialled rings and is answered."""
import json
import os
import socket
import sys
import time

path = sys.argv[1]
if os.path.exists(path):
    os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(1)


def send(c, **kw):
    j = json.dumps(kw)
    c.sendall(f"{len(j)}:{j},".encode())


while True:
    c, _ = srv.accept()
    for aor in sys.argv[2:]:
        send(c, event=True, type="REGISTER_OK", accountaor=aor, param="200 OK")
    buf = b""
    try:
        while True:
            d = c.recv(4096)
            if not d:
                break
            buf += d
            while b":" in buf:
                n, rest = buf.split(b":", 1)
                if len(rest) < int(n) + 1:
                    break
                msg, buf = json.loads(rest[:int(n)]), rest[int(n) + 1:]
                send(c, response=True, ok=True, token=msg.get("token", ""), data="")
                if msg.get("command") == "dial":
                    call = dict(event=True, id="call-1", direction="outgoing", accountaor=sys.argv[2],
                                peeruri=f"sip:{msg.get('params')}@pbx.example.org", param="")
                    send(c, type="CALL_RINGING", **call)
                    time.sleep(0.4)
                    send(c, type="CALL_ESTABLISHED", **call)
                elif msg.get("command") == "hangup":
                    send(c, event=True, type="CALL_CLOSED", id="call-1", direction="outgoing", accountaor=sys.argv[2], param="")
    except OSError:
        pass
    c.close()
