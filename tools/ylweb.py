#!/usr/bin/env python3
"""Minimal client for the Yealink T4xS web UI (firmware 66.86.x).

Reproduces the browser login: a random AES-128 key and IV (hex strings) are
RSA PKCS#1 v1.5 encrypted with the page's public key, and the password is
AES-CBC encrypted (zero padding) as "<nonce>;<JSESSIONID>;<password>".
"""
import base64, os, random, re, sys, urllib3
import requests
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

urllib3.disable_warnings()


def rsa_pkcs1_encrypt(msg: bytes, n: int, e: int) -> str:
    k = (n.bit_length() + 7) // 8
    ps = bytes(random.randint(1, 255) for _ in range(k - 3 - len(msg)))
    m = int.from_bytes(b"\x00\x02" + ps + b"\x00" + msg, "big")
    h = format(pow(m, e, n), "x")
    return h if len(h) % 2 == 0 else "0" + h


def aes_cbc_zero(key: bytes, iv: bytes, data: bytes) -> str:
    data += b"\x00" * (-len(data) % 16)
    enc = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    return base64.b64encode(enc.update(data) + enc.finalize()).decode()


class Phone:
    def __init__(self, host, user, password):
        self.base = host if host.startswith("http") else f"https://{host}"
        self.s = requests.Session()
        self.s.verify = False
        self.user, self.password = user, password
        self.token = None
        self.keyhex = self.ivhex = self.rsakey = self.rsaiv = None

    def url(self, q):
        return f"{self.base}/servlet?{q}"

    def _init_crypto(self, html):
        n = int(re.search(r'g_rsa_n = "([0-9A-Fa-f]+)"', html).group(1), 16)
        e = int(re.search(r'g_rsa_e = "([0-9A-Fa-f]+)"', html).group(1), 16)
        self.n, self.e = n, e
        self.keyhex = os.urandom(16).hex()
        self.ivhex = os.urandom(16).hex()
        self.rsakey = rsa_pkcs1_encrypt(self.keyhex.encode(), n, e)
        self.rsaiv = rsa_pkcs1_encrypt(self.ivhex.encode(), n, e)

    def encrypt(self, value):
        sid = self.s.cookies.get("JSESSIONID")
        plain = f"{random.random()};{sid};{value}".encode()
        return aes_cbc_zero(bytes.fromhex(self.keyhex), bytes.fromhex(self.ivhex), plain)

    def login(self):
        r = self.s.get(self.url("m=mod_listener&p=login&q=loginForm&jumpto=status"), timeout=15)
        self._init_crypto(r.text)
        body = {
            "username": self.user,
            "pwd": self.encrypt(self.password),
            "rsakey": self.rsakey,
            "rsaiv": self.rsaiv,
        }
        r = self.s.post(self.url("m=mod_listener&p=login&q=login"), data=body, timeout=15)
        ok = '"authstatus":"done"' in r.text.replace(" ", "")
        if ok:
            self.refresh_token()
        return ok, r.text[:300]

    def refresh_token(self, page="m=mod_data&p=status&q=load"):
        r = self.s.get(self.url(page), timeout=15)
        m = re.search(r'g_strToken\s*=\s*"([0-9a-fA-F]+)"', r.text)
        if m:
            self.token = m.group(1)
        return r

    def rsa_encrypt(self, value):
        """YLForm.RSAEncrypt: RSA of "<JSESSIONID>;<value>"."""
        sid = self.s.cookies.get("JSESSIONID")
        return rsa_pkcs1_encrypt(f"{sid};{value}".encode(), self.n, self.e)

    def upgrade(self, rom_path, poll_secs=600):
        """Upload a firmware .rom the way the Upgrade page does; returns final status."""
        import time
        r = self.post("m=mod_upgrade&q=beforeupgrade", {})
        print("beforeupgrade:", r.text.strip()[:200])
        q = ("m=mod_upgrade&q=upgrade&native=true&maxlength="
             + requests.utils.quote(self.rsa_encrypt("100MB"), safe=""))
        with open(rom_path, "rb") as f:
            r = self.s.post(self.url(q), files={"UploadName": (os.path.basename(rom_path), f,
                            "application/octet-stream")}, timeout=600)
        print("upload:", r.status_code, re.sub(r"\s+", " ", r.text)[:300])
        t0 = time.time()
        while time.time() - t0 < poll_secs:
            time.sleep(5)
            try:
                st = self.get("m=mod_upgrade&p=settings-upgrade&q=askUpgrade&checkupload=true").text.strip()
            except Exception as ex:
                st = f"(no answer: {type(ex).__name__})"
            print(f"[{int(time.time() - t0):4d}s] askUpgrade: {st[:120]}", flush=True)
            if st.lstrip("-").isdigit() and int(st) < 0:
                return st
            if st.startswith("(no answer"):
                return "phone stopped answering (probably rebooting into the new image)"
        return "timeout"

    def get(self, q):
        return self.s.get(self.url(q), timeout=30)

    def post(self, q, data):
        data = dict(data)
        if self.token:
            data.setdefault("token", self.token)
        return self.s.post(self.url(q), data=data, timeout=30)


if __name__ == "__main__":
    host, user, pw = sys.argv[1:4]
    p = Phone(host, user, pw)
    ok, txt = p.login()
    print("login ok:", ok)
    print(txt)
    print("token:", p.token)
