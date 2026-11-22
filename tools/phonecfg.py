"""Where the phone is and how to log in. Password lives outside the repo."""
import os
HOST = os.environ.get("T48S_HOST", "10.45.0.217")
_cred = os.environ.get("T48S_LOGIN") or open(os.path.expanduser("~/.config/yealink-t48s/web-login")).read().strip()
USER, PASSWORD = _cred.split(":", 1)


def my_ip():
    """This machine's address as seen from the phone's network."""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((HOST, 9))
    ip = s.getsockname()[0]
    s.close()
    return ip
