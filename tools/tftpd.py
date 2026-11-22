#!/usr/bin/env python3
"""tftpd.py DIR [PORT]: minimal read-only TFTP server for U-Boot's recovery boot.

Serves files from DIR (octet mode, RFC 2347/2348/2349 options blksize, tsize,
timeout). Logs every request, so we can see the exact names U-Boot asks for.
A request for any *.rom that does not exist is answered with ROM_FALLBACK
(the recovery mode may name the ROM after an internal board type).
Needs root for port 69.
"""
import os, socket, struct, sys, threading, time

ROOT = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 69
ROM_FALLBACK = "T48S.rom"


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def serve(client, filename, opts):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))
    name = os.path.basename(filename.replace("\\", "/"))
    path = os.path.join(ROOT, name)
    if not os.path.isfile(path) and name.lower().endswith(".rom"):
        log(f"{client[0]}: {name} not found, serving {ROM_FALLBACK}")
        path = os.path.join(ROOT, ROM_FALLBACK)
    if not os.path.isfile(path):
        log(f"{client[0]}: {name} not found")
        s.sendto(struct.pack("!HH", 5, 1) + b"File not found\0", client)
        return
    data = open(path, "rb").read()
    blksize, timeout = 512, 2.0
    oack = {}
    if "blksize" in opts:
        blksize = max(8, min(int(opts["blksize"]), 1468))
        oack["blksize"] = str(blksize)
    if "tsize" in opts:
        oack["tsize"] = str(len(data))
    if "timeout" in opts:
        timeout = float(opts["timeout"])
        oack["timeout"] = opts["timeout"]
    s.settimeout(timeout)
    log(f"{client[0]}: sending {name} ({len(data)} bytes, blksize {blksize})")

    def send_wait(pkt, want_block):
        # Resend on timeout, and also when the client repeats its previous ACK
        # (it does that when it missed our packet); otherwise wait for the right ACK.
        for _ in range(12):
            s.sendto(pkt, client)
            deadline = time.time() + timeout
            try:
                while True:
                    s.settimeout(max(0.05, deadline - time.time()))
                    r, addr = s.recvfrom(1024)
                    if addr != client:
                        continue
                    op, blk = struct.unpack("!HH", r[:4])
                    if op == 4 and blk == want_block:
                        return True
                    if op == 5:
                        log(f"{client[0]}: client error {r[4:].decode(errors='replace')}")
                        return False
                    if op == 4 and blk == (want_block - 1) & 0xFFFF:
                        break  # duplicate ACK: resend now
                    if time.time() >= deadline:
                        break
            except socket.timeout:
                continue
        log(f"{client[0]}: gave up waiting for ack {want_block}")
        return False

    if oack:
        pkt = struct.pack("!H", 6) + b"".join(k.encode() + b"\0" + v.encode() + b"\0" for k, v in oack.items())
        if not send_wait(pkt, 0):
            return
    block, off, t0 = 1, 0, time.time()
    while True:
        chunk = data[off:off + blksize]
        if not send_wait(struct.pack("!HH", 3, block & 0xFFFF) + chunk, block & 0xFFFF):
            return
        off += len(chunk)
        block += 1
        if len(chunk) < blksize:
            break
    log(f"{client[0]}: sent {name} in {time.time() - t0:.1f} s")


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", PORT))
    log(f"serving {ROOT} on udp/{PORT}: {sorted(os.listdir(ROOT))}")
    while True:
        pkt, client = s.recvfrom(1024)
        if len(pkt) < 4 or struct.unpack("!H", pkt[:2])[0] != 1:
            continue
        parts = pkt[2:].split(b"\0")
        filename, mode = parts[0].decode(errors="replace"), parts[1].decode(errors="replace").lower()
        opts = {parts[i].decode().lower(): parts[i + 1].decode() for i in range(2, len(parts) - 1, 2) if parts[i]}
        log(f"{client[0]}:{client[1]} read request {filename!r} mode {mode} options {opts}")
        threading.Thread(target=serve, args=(client, filename, opts), daemon=True).start()


main()
