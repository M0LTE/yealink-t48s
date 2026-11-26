# SPDX-License-Identifier: AGPL-3.0-or-later
"""fakesip.py SOCKET AOR...: stands in for baresip's control socket in the small-screen tests,
so that the call screens can be reached with no SIP service. Every account is registered. A
call that is dialled rings for a second and a half and is answered. A line written to
SOCKET.cmd is done and the file removed:
    incoming NUMBER [NAME]    a call comes in
    secure                    every call's audio is now SRTP (CALL_MENC, issue #78)"""
import json
import os
import select
import socket
import sys
import time

path, aors = sys.argv[1], sys.argv[2:]
cmd = path + ".cmd"
if os.path.exists(path):
    os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(1)
calls = {}      # id -> the event fields of that call
answer_at = {}  # id -> when a dialled call is answered
serial = 0


def send(c, **kw):
    j = json.dumps(kw)
    c.sendall(f"{len(j)}:{j},".encode())


def new_call(direction, number, name=""):
    global serial
    serial += 1
    call = dict(event=True, id=f"call-{serial}", direction=direction, accountaor=aors[0],
                peeruri=f"sip:{number}@pbx.example.org", param="")
    if name:
        call["peerdisplayname"] = name
    calls[call["id"]] = call
    return call


while True:
    c, _ = srv.accept()
    for aor in aors:
        send(c, event=True, type="REGISTER_OK", accountaor=aor, param="200 OK")
    buf = b""
    try:
        while True:
            now = time.time()
            for cid in [k for k, t in answer_at.items() if t <= now]:
                del answer_at[cid]
                if cid in calls:
                    send(c, type="CALL_ESTABLISHED", **calls[cid])
            if os.path.exists(cmd):
                words = open(cmd).read().split()
                os.unlink(cmd)
                if words and words[0] == "incoming":
                    send(c, type="CALL_INCOMING", **new_call("incoming", words[1], " ".join(words[2:])))
                elif words and words[0] == "secure":
                    for call in calls.values():
                        send(c, type="CALL_MENC", **dict(call, param="0,audio,AES_CM_128_HMAC_SHA1_80"))
            if not select.select([c], [], [], 0.05)[0]:
                continue
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
                what, arg = msg.get("command"), msg.get("params") or ""
                if what == "dial":
                    call = new_call("outgoing", arg)
                    send(c, type="CALL_RINGING", **call)
                    answer_at[call["id"]] = time.time() + 1.5
                elif what == "accept" and arg in calls:
                    send(c, type="CALL_ESTABLISHED", **calls[arg])
                elif what == "hangup":
                    for cid in ([arg] if arg in calls else list(calls)):
                        send(c, type="CALL_CLOSED", **calls.pop(cid))
                        answer_at.pop(cid, None)
    except OSError:
        pass
    c.close()
    calls.clear()
    answer_at.clear()
