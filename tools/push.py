# SPDX-License-Identifier: AGPL-3.0-or-later
"""push.py LOCAL REMOTE: copy a file to the phone over the root shell (phone pulls via busybox nc)."""
import socket, sys, threading
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from tsh import Shell
from phonecfg import my_ip
local, remote = sys.argv[1], sys.argv[2]
data = open(local, "rb").read()
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", 9124)); srv.listen(1)
def serve():
    c, _ = srv.accept(); c.sendall(data); c.close()
threading.Thread(target=serve, daemon=True).start()
out, rc = Shell().run(f"busybox nc {my_ip()} 9124 > {remote} && ls -l {remote}")
print(out, end="")
