#!/usr/bin/env python3
"""Client for the older Yealink T4xS web UI (66.82.x, plain HTTP)."""
import hashlib, os, random, re, sys
import requests
from ylweb import rsa_pkcs1_encrypt, aes_cbc_zero


class OldPhone:
    def __init__(self, base, user, password):
        self.base = base.rstrip("/")
        self.s = requests.Session()
        self.s.verify = False
        self.user, self.password = user, password
        self.token = ""

    def url(self, q):
        return f"{self.base}/servlet?{q}"

    def login(self):
        r = self.s.get(self.url("p=login&q=getsessionid"), timeout=15)
        n_hex, e_hex, sid = r.text.strip().split(",")
        n, e = int(n_hex, 16), int(e_hex, 16)
        self.s.cookies.set("JSESSIONID", sid)
        key, iv = os.urandom(16).hex(), os.urandom(16).hex()
        data = (f"rand={random.random()};sessionid={sid};"
                f"username={self.user};pwd={self.password};")
        data = "MD5=" + hashlib.md5(data.encode()).hexdigest() + ";" + data
        form = {
            "key": rsa_pkcs1_encrypt(key.encode(), n, e),
            "iv": rsa_pkcs1_encrypt(iv.encode(), n, e),
            "data": aes_cbc_zero(bytes.fromhex(key), bytes.fromhex(iv), data.encode()),
        }
        r = self.s.post(self.url("p=login&q=login"), data=form, timeout=15)
        self.last = r
        self.refresh_token()
        return bool(self.token), r.status_code

    def refresh_token(self, q="p=status&q=load"):
        r = self.s.get(self.url(q), timeout=15)
        m = re.search(r'g_strToken\s*=\s*"([0-9a-fA-F]+)"', r.text)
        self.token = m.group(1) if m else ""
        return r

    def get(self, q):
        return self.s.get(self.url(q), timeout=30)

    def post(self, q, data):
        data = dict(data)
        if self.token:
            data.setdefault("token", self.token)
        return self.s.post(self.url(q), data=data, timeout=30)


if __name__ == "__main__":
    p = OldPhone(sys.argv[1], sys.argv[2], sys.argv[3])
    print("login:", p.login(), "token:", p.token)
