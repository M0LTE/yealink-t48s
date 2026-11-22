#!/usr/bin/env python3
"""Run shell commands on the phone over the busybox telnetd root shell.

Usage: tsh.py 'command' ['command' ...]   (host/port via TSH_HOST / TSH_PORT)
"""
import os, re, socket, sys, time, uuid

from phonecfg import HOST
PORT = int(os.environ.get("TSH_PORT", "2323"))
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


class Shell:
    def __init__(self, host=HOST, port=PORT):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.buf = b""
        time.sleep(0.5)
        self._read_avail()
        self.buf = b""
        self.run("stty -echo 2>/dev/null; PS1=''; export PS1", timeout=5)

    def _filter(self, data):
        out, i = bytearray(), 0
        while i < len(data):
            b = data[i]
            if b == IAC and i + 1 < len(data):
                cmd = data[i + 1]
                if cmd in (DO, DONT, WILL, WONT) and i + 2 < len(data):
                    opt = data[i + 2]
                    reply = WONT if cmd in (DO, DONT) else DONT
                    self.sock.sendall(bytes([IAC, reply, opt]))
                    i += 3
                    continue
                if cmd == SB:
                    j = data.find(bytes([IAC, SE]), i)
                    i = len(data) if j < 0 else j + 2
                    continue
                i += 2
                continue
            out.append(b)
            i += 1
        return bytes(out)

    def _read_avail(self, wait=0.2):
        self.sock.settimeout(wait)
        try:
            while True:
                d = self.sock.recv(65536)
                if not d:
                    break
                self.buf += self._filter(d)
        except socket.timeout:
            pass

    def run(self, cmd, timeout=60):
        tag = uuid.uuid4().hex[:12]
        self.sock.sendall(f"{cmd}; echo __END_{tag}_$?__\n".encode())
        end = time.time() + timeout
        pat = re.compile(rf"__END_{tag}_(\d+)__".encode())
        while time.time() < end:
            self._read_avail()
            m = pat.search(self.buf)
            if m:
                out = self.buf[: m.start()]
                self.buf = self.buf[m.end():]
                text = out.decode("latin1").replace("\r", "")
                # drop the echoed copy of the command (the shell has no pty, so it echoes)
                text = "\n".join(l for l in text.split("\n") if f"__END_{tag}_" not in l)
                return text.lstrip("\n"), int(m.group(1))
        raise TimeoutError(cmd)


if __name__ == "__main__":
    sh = Shell()
    for c in sys.argv[1:]:
        out, rc = sh.run(c)
        sys.stdout.write(out)
        if rc:
            print(f"[exit {rc}]")
