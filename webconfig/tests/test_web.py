#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Tests for t48-web (docs/web.md): the real program, over HTTP, against a
throwaway settings tree. python3 tests/test_web.py BUILD_DIR

Pairing, the refusals (not paired, no token, another site's Origin), every
form, and the backup round trip. A stand-in for t48-apply writes down what it
was asked to do. RUNNER=qemu-arm SHELL_BUILD=../shell/build runs the phone's
own ARM build of the server the same way. The shell's own t48-app installs from the test feed on disk.
"""
import http.client, os, re, shutil, socket, subprocess, sys, tempfile, time, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.abspath(sys.argv.pop(1) if len(sys.argv) > 1 else "build")
# RUNNER=qemu-arm runs the phone's own build of t48-web; the shell's tools stay this PC's (SHELL_BUILD)
SHELL_BUILD = os.environ.get("SHELL_BUILD") or os.path.join(HERE, "..", "..", "shell", os.path.basename(BUILD))
RUNNER = os.environ.get("RUNNER", "").split()
WEB = os.path.join(BUILD, "t48-web")
KEY1 = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOeStCLCiE0njMGEkRNUHaNmd+dNxoLC9NECKxV/wLQ9 tom@desk"
KEY2 = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFf1h3n4kq0m3Qy0dJm1o0mC2aQy8vHk1W9cJ8H2Yb7Q laptop"


def read(path):
    with open(path) as f:
        return f.read()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Phone:
    """A settings tree and a t48-web serving it."""

    def __init__(self, enabled=True):
        self.dir = tempfile.mkdtemp(prefix="t48web-")
        self.config = os.path.join(self.dir, "config")
        self.state = os.path.join(self.dir, "phone")
        self.run = os.path.join(self.dir, "run")
        self.keep = os.path.join(self.dir, "keep")
        self.access = os.path.join(self.dir, "access")
        self.installed = os.path.join(self.dir, "installed")
        for d in (self.config, self.state, self.access, self.installed):
            os.makedirs(d)
        self.applied = os.path.join(self.dir, "applied")
        self.apply = os.path.join(self.dir, "apply.sh")
        with open(self.apply, "w") as f:
            f.write('#!/bin/sh\necho "$1" >> "%s"\n' % self.applied)
        os.chmod(self.apply, 0o755)
        # the stand-in for t48-update (update.sh) keeps what it knows in a folder; "restart" only says it was asked
        self.fake = os.path.join(self.dir, "update")
        os.makedirs(self.fake)
        self.update = os.path.join(self.dir, "update.sh")
        with open(self.update, "w") as f:
            f.write('#!/bin/sh\nFAKE="%s" exec sh "%s" "$@"\n' % (self.fake, os.path.join(HERE, "update.sh")))
        os.chmod(self.update, 0o755)
        self.restart = os.path.join(self.dir, "restart.sh")
        with open(self.restart, "w") as f:
            f.write('#!/bin/sh\necho restart >> "%s"\n' % os.path.join(self.fake, "restarted"))
        os.chmod(self.restart, 0o755)
        if enabled:
            self.set("access", "web", "on")
        self.port = free_port()
        self.log = os.path.join(self.dir, "log")
        self.proc = None

    def start(self):
        self.logfile = open(self.log, "w")
        self.proc = subprocess.Popen(
            RUNNER + [WEB, "--listen", "127.0.0.1:%d" % self.port, "--config", self.config, "--state", self.state,
             "--apps", self.installed + ":" + os.path.join(SHELL_BUILD, "apps"), "--data", os.path.join(self.dir, "appdata"),
             "--apply", self.apply, "--app", os.path.join(SHELL_BUILD, "t48-app"), "--access", self.access,
             "--keys", self.keep, "--run", self.run, "--update", self.update, "--restart", self.restart],
            stderr=self.logfile)
        for _ in range(100):
            if self.proc.poll() is not None or (os.path.exists(self.log) and "listening" in read(self.log)):
                break
            time.sleep(0.05)
        return self

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(5)
        if self.proc:
            self.logfile.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def cfg(self, *args):
        return subprocess.run([os.path.join(SHELL_BUILD, "t48-config"), "--config", self.config] + list(args),
                              capture_output=True, text=True)

    def get(self, file, key):
        r = self.cfg("get", file, key)
        return r.stdout.rstrip("\n") if r.returncode == 0 else None

    def set(self, file, key, value):
        assert self.cfg("set", file, key, value).returncode == 0

    def status(self):
        try:
            return dict(l.split("=", 1) for l in read(os.path.join(self.run, "status")).splitlines() if "=" in l)
        except FileNotFoundError:
            return {}

    def logtext(self):
        return read(self.log)

    def applied_list(self):
        return read(self.applied).split() if os.path.exists(self.applied) else []


class Browser:
    def __init__(self, phone):
        self.phone = phone
        self.cookies = {}
        self.csrf = ""

    def request(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection("127.0.0.1", self.phone.port, timeout=200)
        h = {"Host": "127.0.0.1:%d" % self.phone.port}
        if self.cookies:
            h["Cookie"] = "; ".join("%s=%s" % kv for kv in self.cookies.items())
        if headers:
            h.update(headers)
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        for k, v in r.getheaders():
            if k.lower() == "set-cookie":
                name, value = v.split(";")[0].split("=", 1)
                if value:
                    self.cookies[name] = value
                else:
                    self.cookies.pop(name, None)
        c.close()
        r.raw = data
        r.body = data.decode("utf-8", "replace")
        m = re.search(r"name='csrf' value='([0-9a-f]+)'", r.body)
        if m:
            self.csrf = m.group(1)
        return r

    def get(self, path):
        return self.request("GET", path)

    def post(self, path, fields=None, csrf=True, headers=None):
        from urllib.parse import urlencode
        f = dict(fields or {})
        if csrf:
            f["csrf"] = self.csrf
        h = {"Content-Type": "application/x-www-form-urlencoded"}
        h.update(headers or {})
        return self.request("POST", path, urlencode(f), h)

    def upload(self, path, fields, filename, content):
        b = "----t48test"
        parts = []
        f = dict(fields)
        f["csrf"] = self.csrf
        for k, v in f.items():
            parts.append('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n%s\r\n' % (b, k, v))
        parts.append('--%s\r\nContent-Disposition: form-data; name="file"; filename="%s"\r\nContent-Type: text/plain\r\n\r\n%s\r\n'
                     % (b, filename, content))
        parts.append("--%s--\r\n" % b)
        return self.request("POST", path, "".join(parts).encode(), {"Content-Type": "multipart/form-data; boundary=" + b})

    def pair(self):
        r = self.post("/pair/start", csrf=False)
        assert r.status == 303 and r.getheader("Location") == "/pair", (r.status, r.getheader("Location"))
        code = self.phone.status()["pair_code"]
        r = self.post("/pair/finish", {"code": code}, csrf=False)
        assert r.status == 303 and r.getheader("Location") == "/", (r.status, r.getheader("Location"))
        self.get("/")
        assert self.csrf
        return self

    def flash(self, path):
        """the message the last change left, from the next page"""
        m = re.search(r"<p class='flash( bad)?' role='status'>(.*?)</p>", self.get(path).body)
        return (bool(m.group(1)), m.group(2)) if m else (None, "")


class Off(unittest.TestCase):
    def test_does_not_run_until_turned_on(self):
        p = Phone(enabled=False).start()
        try:
            self.assertEqual(p.proc.wait(5), 0)
            self.assertIn("the web page is off", p.logtext())
            with self.assertRaises(OSError):
                socket.create_connection(("127.0.0.1", p.port), 1)
        finally:
            p.stop()

    def test_leaves_when_turned_off_at_the_phone(self):
        p = Phone().start()
        try:
            self.assertEqual(Browser(p).get("/").status, 200)
            self.assertTrue(p.status().get("url", "").startswith("http://127.0.0.1"))
            p.set("access", "web", "off")
            self.assertEqual(p.proc.wait(5), 0)
            self.assertEqual(p.status(), {})
        finally:
            p.stop()


class Pairing(unittest.TestCase):
    def setUp(self):
        self.p = Phone().start()

    def tearDown(self):
        self.p.stop()

    def test_nothing_without_pairing(self):
        b = Browser(self.p)
        r = b.get("/")
        self.assertEqual(r.status, 200)
        self.assertIn("Show code", r.body)
        self.assertIn("Plain HTTP", r.body)
        self.assertEqual(r.getheader("X-Frame-Options"), "DENY")
        self.assertIn("default-src 'none'", r.getheader("Content-Security-Policy"))
        for path in ("/accounts", "/backup", "/contacts/export.csv", "/access", "/update"):
            r = b.get(path)
            self.assertEqual((r.status, r.getheader("Location")), (303, "/"), path)
        self.p.set("sip", "user", "1001")
        for path in ("/accounts/save", "/backup/download", "/access/ssh", "/access/key-add"):
            r = b.post(path, {"n": "1", "user": "evil", "to": "on", "keys": KEY1}, csrf=False)
            self.assertEqual(r.status, 403, path)
        self.assertEqual(self.p.get("sip", "user"), "1001")
        self.assertIsNone(self.p.get("access", "ssh"))
        for path in ("/update/check", "/update/install", "/update/kernel", "/update/restart", "/update/channel"):
            r = b.post(path, {"sure": "yes", "channel": "testing"}, csrf=False)
            self.assertEqual(r.status, 403, path)
        self.assertFalse(os.path.exists(os.path.join(self.p.fake, "calls")))
        self.assertEqual(b.get("/style.css").status, 200)
        self.assertEqual(b.get("/nothing").status, 303)

    def test_pair_with_the_code_on_the_screen(self):
        b = Browser(self.p)
        self.assertNotIn("pair_code", self.p.status())
        r = b.post("/pair/start", csrf=False)
        self.assertEqual(r.getheader("Location"), "/pair")
        st = self.p.status()
        self.assertRegex(st["pair_code"], r"^\d{6}$")
        self.assertEqual(st["pair_from"], "127.0.0.1")
        self.assertGreater(int(st["pair_until"]), time.time() + 60)
        self.assertEqual(os.stat(os.path.join(self.p.run, "status")).st_mode & 0o777, 0o600)
        self.assertIn("Enter code", b.get("/pair").body)
        # the code is never in a page or the log
        self.assertNotIn(st["pair_code"], b.get("/pair").body)
        self.assertNotIn(st["pair_code"], self.p.logtext())
        # another browser that has seen the code cannot use it: it did not ask for it
        other = Browser(self.p)
        r = other.post("/pair/finish", {"code": st["pair_code"]}, csrf=False)
        self.assertEqual(r.getheader("Location"), "/?e=gone")
        self.assertNotIn("t48s", other.cookies)
        # a wrong one
        wrong = "%06d" % ((int(st["pair_code"]) + 1) % 1000000)
        r = b.post("/pair/finish", {"code": wrong}, csrf=False)
        self.assertEqual(r.getheader("Location"), "/pair?e=wrong")
        self.assertIn("4 tries left", b.get("/pair?e=wrong").body)
        # the right one, typed with the gap the screen shows
        code = st["pair_code"]
        r = b.post("/pair/finish", {"code": code[:3] + " " + code[3:]}, csrf=False)
        self.assertEqual(r.getheader("Location"), "/")
        self.assertIn("t48s", b.cookies)
        self.assertRegex(r.getheader("Set-Cookie"), r"HttpOnly; SameSite=Strict")
        st = self.p.status()
        self.assertEqual(st["paired"], "1")
        self.assertNotIn("pair_code", st)
        r = b.get("/")
        self.assertIn("Overview", r.body)
        self.assertIn("Plain HTTP", r.body)
        # the cookie is not in the log either
        self.assertNotIn(b.cookies["t48s"], self.p.logtext())
        # signing out
        r = b.post("/logout")
        self.assertEqual(r.getheader("Location"), "/?e=out")
        self.assertEqual(self.p.status()["paired"], "0")
        self.assertEqual(b.get("/accounts").status, 303)

    def test_five_wrong_tries_and_the_code_is_gone(self):
        b = Browser(self.p)
        b.post("/pair/start", csrf=False)
        code = self.p.status()["pair_code"]
        wrong = "%06d" % ((int(code) + 7) % 1000000)
        for i in range(4):
            self.assertEqual(b.post("/pair/finish", {"code": wrong}, csrf=False).getheader("Location"), "/pair?e=wrong")
        self.assertEqual(b.post("/pair/finish", {"code": wrong}, csrf=False).getheader("Location"), "/?e=tries")
        self.assertNotIn("pair_code", self.p.status())
        self.assertEqual(b.post("/pair/finish", {"code": code}, csrf=False).getheader("Location"), "/?e=gone")
        self.assertNotIn("t48s", b.cookies)

    def test_asking_again_at_once_is_refused(self):
        b = Browser(self.p)
        self.assertEqual(b.post("/pair/start", csrf=False).getheader("Location"), "/pair")
        first = self.p.status()["pair_code"]
        self.assertEqual(b.post("/pair/start", csrf=False).getheader("Location"), "/?e=wait")
        self.assertEqual(self.p.status()["pair_code"], first)

    def test_refused_at_the_phone(self):
        b = Browser(self.p)
        b.post("/pair/start", csrf=False)
        code = self.p.status()["pair_code"]
        with open(os.path.join(self.p.run, "refuse"), "w") as f:
            f.write(code + "\n")
        self.assertEqual(b.post("/pair/finish", {"code": code}, csrf=False).getheader("Location"), "/?e=gone")
        self.assertNotIn("pair_code", self.p.status())

    def test_fifteen_wrong_tries_close_pairing(self):
        b = Browser(self.p)
        for round_ in range(3):
            self.assertEqual(b.post("/pair/start", csrf=False).getheader("Location"), "/pair")
            wrong = "%06d" % ((int(self.p.status()["pair_code"]) + 3) % 1000000)
            for i in range(5):
                last = b.post("/pair/finish", {"code": wrong}, csrf=False).getheader("Location")
            time.sleep(3.1)
        self.assertEqual(last, "/?e=locked")
        self.assertEqual(b.post("/pair/start", csrf=False).getheader("Location"), "/?e=locked")
        self.assertNotIn("pair_code", self.p.status())
        self.assertIn("locked for 10 minutes", b.get("/?e=locked").body)
        self.assertIn("closed for 10 minutes", self.p.logtext())

    def test_another_site_cannot_ask_for_a_code(self):
        b = Browser(self.p)
        r = b.post("/pair/start", csrf=False, headers={"Origin": "http://evil.example"})
        self.assertEqual(r.status, 403)
        self.assertNotIn("pair_code", self.p.status())

    def test_too_big(self):
        b = Browser(self.p)
        c = http.client.HTTPConnection("127.0.0.1", self.p.port, timeout=10)
        c.putrequest("POST", "/pair/finish")
        c.putheader("Content-Length", str(10 * 1024 * 1024))
        c.endheaders()
        r = c.getresponse()
        r.read()
        c.close()
        self.assertEqual(r.status, 413)


class Paired(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.p = Phone()
        cls.p.set("feed", "url", "file://" + os.path.join(SHELL_BUILD, "feeds", "1", "index.json"))
        cls.p.start()
        cls.b = Browser(cls.p).pair()

    @classmethod
    def tearDownClass(cls):
        cls.p.stop()

    def ok(self, path, fields, back):
        r = self.b.post(path, fields)
        self.assertEqual(r.status, 303, r.body)
        bad, text = self.b.flash(back)
        self.assertIs(bad, False, text)
        return text

    def bad(self, path, fields, back):
        r = self.b.post(path, fields)
        self.assertEqual(r.status, 303, r.body)
        bad, text = self.b.flash(back)
        self.assertIs(bad, True, text)
        return text

    # ---- the refusals ----

    def test_00_csrf(self):
        b, p = self.b, self.p
        fields = {"n": "2", "user": "mallory", "domain": "evil.example", "transport": "udp", "enabled": "on"}
        # no token, a wrong token, the right token from another site
        self.assertEqual(b.post("/accounts/save", fields, csrf=False).status, 403)
        self.assertEqual(b.post("/accounts/save", dict(fields, csrf="0" * 64), csrf=False).status, 403)
        self.assertEqual(b.post("/accounts/save", fields, headers={"Origin": "http://evil.example"}).status, 403)
        self.assertEqual(b.post("/accounts/save", fields, headers={"Origin": "null"}).status, 403)
        self.assertIsNone(p.get("sip2", "user"))
        # a GET changes nothing
        self.assertEqual(b.get("/accounts/save?n=2&user=mallory").status, 404)
        self.assertEqual(b.get("/history/clear").status, 404)
        # from this page itself
        self.assertEqual(b.post("/accounts/save", fields, headers={"Origin": "http://127.0.0.1:%d" % p.port}).status, 303)
        self.assertEqual(p.get("sip2", "user"), "mallory")
        self.ok("/accounts/remove", {"n": "2", "sure": "yes"}, "/accounts")
        self.assertIn("token is missing or wrong", p.logtext())
        self.assertIn("sent from another site", p.logtext())

    # ---- every form ----

    def test_10_accounts(self):
        b, p = self.b, self.p
        secret = "s3cret-Pass word&<>'\"%"
        text = self.ok("/accounts/save", {"n": "1", "user": "1001", "domain": "pbx.example.org", "password": secret,
                                          "outbound_proxy": "", "auth_user": "tom", "display_name": "Tom <M0LTE> & Co", "transport": "tls",
                                          "label": "Home", "voicemail": "*97", "enabled": "on"}, "/accounts")
        self.assertIn("Account 1 saved", text)
        self.assertEqual(p.get("sip", "user"), "1001")
        self.assertEqual(p.get("sip", "domain"), "pbx.example.org")
        self.assertEqual(p.get("sip", "password"), secret)
        self.assertEqual(p.get("sip", "transport"), "tls")
        self.assertEqual(p.get("sip", "display_name"), "Tom <M0LTE> & Co")
        self.assertEqual(p.get("sip", "enabled"), "on")
        self.assertIsNone(p.get("sip", "outbound_proxy"))
        self.assertEqual(os.stat(os.path.join(p.config, "sip")).st_mode & 0o777, 0o600)
        self.assertIn("sip", p.applied_list())
        page = b.get("/accounts").body
        # write only: the page says it is set and never what it is; what is shown is escaped
        self.assertNotIn("s3cret", page)
        self.assertIn("Set. Empty keeps it", page)
        self.assertIn("Tom &lt;M0LTE&gt; &amp; Co", page)
        self.assertNotIn("<M0LTE>", page)
        # an empty password keeps the old one
        self.ok("/accounts/save", {"n": "1", "user": "1001", "domain": "pbx.example.org", "password": "", "transport": "udp", "label": "Home"}, "/accounts")
        self.assertEqual(p.get("sip", "password"), secret)
        self.assertEqual(p.get("sip", "enabled"), "off")
        self.assertIsNone(p.get("sip", "auth_user"))
        self.ok("/accounts/save", {"n": "1", "user": "1001", "domain": "pbx.example.org", "transport": "udp", "enabled": "on"}, "/accounts")
        # the sixth account, and which is usual
        self.ok("/accounts/save", {"n": "6", "user": "2002", "domain": "other.example", "password": "pw6-hunter2", "transport": "tcp", "enabled": "on"}, "/accounts")
        self.assertEqual(p.get("sip6", "user"), "2002")
        self.assertIn("Default account", b.get("/accounts").body)
        self.ok("/accounts/default", {"n": "6"}, "/accounts")
        self.assertEqual(p.get("phone", "default_account"), "6")
        # refusals
        self.bad("/accounts/save", {"n": "2", "user": "x" * 300, "domain": "d", "transport": "udp"}, "/accounts")
        self.assertIsNone(p.get("sip2", "user"))
        self.assertEqual(b.post("/accounts/save", {"n": "7", "user": "x"}).status, 303)
        self.assertFalse(os.path.exists(os.path.join(p.config, "sip7")))
        self.bad("/accounts/remove", {"n": "6"}, "/accounts")          # the box was not ticked
        self.assertEqual(p.get("sip6", "user"), "2002")
        self.ok("/accounts/remove", {"n": "6", "sure": "yes"}, "/accounts")
        self.assertFalse(os.path.exists(os.path.join(p.config, "sip6")))
        log = p.logtext()
        self.assertNotIn("s3cret", log)
        self.assertNotIn("hunter2", log)
        self.assertNotIn("pbx.example.org", log)

    def test_20_network(self):
        b, p = self.b, self.p
        before = len(p.applied_list())
        self.bad("/network", {"mode": "static", "address": "10.0.0.300", "netmask": "255.255.255.0"}, "/network")
        self.bad("/network", {"mode": "static", "address": "", "netmask": "255.255.255.0"}, "/network")
        self.bad("/network", {"mode": "static", "address": "10.0.0.5", "netmask": "255.255.255.0", "gateway": "router"}, "/network")
        self.assertIsNone(p.get("network", "mode"))
        self.ok("/network", {"mode": "static", "address": "10.0.0.5", "netmask": "255.255.255.0", "gateway": "10.0.0.1", "dns": "10.0.0.1"}, "/network")
        self.assertEqual([p.get("network", k) for k in ("mode", "address", "netmask", "gateway", "dns")],
                         ["static", "10.0.0.5", "255.255.255.0", "10.0.0.1", "10.0.0.1"])
        self.ok("/network", {"mode": "dhcp", "address": "10.0.0.5", "netmask": "255.255.255.0", "gateway": "10.0.0.1", "dns": ""}, "/network")
        self.assertEqual(p.get("network", "mode"), "dhcp")
        # applied a moment later, so the answer got back first
        time.sleep(2.6)
        self.assertEqual(p.applied_list()[before:].count("network"), 2)

    def test_30_time(self):
        p = self.p
        self.ok("/time", {"zone": "Europe/Berlin", "custom": ""}, "/time")
        self.assertEqual(p.get("system", "timezone"), "Europe/Berlin")
        self.assertIn("<option selected>Europe/Berlin</option>", self.b.get("/time").body)
        self.ok("/time", {"zone": "other", "custom": "XYZ-3"}, "/time")
        self.assertEqual(p.get("system", "timezone"), "XYZ-3")
        self.bad("/time", {"zone": "other", "custom": ""}, "/time")
        self.bad("/time", {"zone": "other", "custom": "not a zone"}, "/time")
        self.assertEqual(p.get("system", "timezone"), "XYZ-3")
        self.ok("/time", {"zone": "Europe/London"}, "/time")

    def test_40_sound_and_screen(self):
        p = self.p
        self.ok("/sound", {"ringer": "3", "handset": "9", "speaker": "12", "headset": "0", "apps": "7"}, "/sound")
        self.assertEqual([p.get("audio", k) for k in ("ringer", "handset", "speaker", "headset", "apps")], ["3", "9", "12", "0", "7"])
        self.bad("/sound", {"ringer": "16", "handset": "9", "speaker": "12", "headset": "0", "apps": "7"}, "/sound")
        self.bad("/sound", {"ringer": "loud", "handset": "9", "speaker": "12", "headset": "0", "apps": "7"}, "/sound")
        self.assertEqual(p.get("audio", "ringer"), "3")
        self.ok("/screen", {"brightness": "4", "dim_seconds": "60", "off_seconds": "0"}, "/sound")
        self.assertEqual([p.get("screen", k) for k in ("brightness", "dim_seconds", "off_seconds")], ["4", "60", "0"])
        self.bad("/screen", {"brightness": "0", "dim_seconds": "60", "off_seconds": "0"}, "/sound")
        self.bad("/screen", {"brightness": "4", "dim_seconds": "-1", "off_seconds": "0"}, "/sound")
        self.assertEqual(p.get("screen", "brightness"), "4")
        page = self.b.get("/sound").body
        self.assertIn("name='ringer' min='0' max='15' value='3'", page)

    def test_41_theme(self):
        p, b = self.p, self.b
        # Switchboard until told otherwise, and the page says which in <html>
        page = b.get("/sound").body
        self.assertIn("<html lang='en' data-theme='switchboard'>", page)
        self.assertIn("value='switchboard' checked", page)
        for name in ("directory", "twotone", "switchboard"):
            self.ok("/theme", {"theme": name}, "/sound")
            self.assertEqual(p.get("screen", "theme"), name)
            for path in ("/sound", "/", "/accounts"):
                self.assertIn("<html lang='en' data-theme='%s'>" % name, b.get(path).body, path)
            self.assertIn("value='%s' checked" % name, b.get("/sound").body)
        self.bad("/theme", {"theme": "plaid"}, "/sound")
        self.bad("/theme", {}, "/sound")
        self.assertEqual(p.get("screen", "theme"), "switchboard")
        # set at the phone (or by hand): the next page wears it; a name that is no theme is Switchboard
        p.set("screen", "theme", "twotone")
        self.assertIn("data-theme='twotone'", b.get("/").body)
        p.set("screen", "theme", "plaid")
        self.assertIn("data-theme='switchboard'", b.get("/").body)
        p.set("screen", "theme", "switchboard")
        # a stranger's pages wear it too, and get the style sheet and fonts without pairing
        p.set("screen", "theme", "directory")
        s = Browser(p)
        self.assertIn("data-theme='directory'", s.get("/").body)
        css = s.get("/style.css")
        self.assertEqual(css.status, 200)
        for name in ("switchboard", "directory", "twotone"):
            self.assertIn("[data-theme=%s]" % name, css.body)
        for family in ("b612", "plex", "figtree"):
            for weight in ("regular", "bold"):
                path = "/fonts/%s-%s.woff2" % (family, weight)
                self.assertIn(path, css.body)
                r = s.get(path)
                self.assertEqual((r.status, r.getheader("Content-Type")), (200, "font/woff2"), path)
                self.assertEqual(r.raw[:4], b"wOF2", path)
        self.assertEqual(s.get("/fonts/none.woff2").status, 404)
        self.assertEqual(s.get("/fonts/../style.css").status, 404)
        p.set("screen", "theme", "switchboard")

    def contacts(self):
        path = os.path.join(self.p.state, "contacts")
        return read(path).splitlines() if os.path.exists(path) else []

    def test_50_contacts(self):
        b = self.b
        self.ok("/contacts/save", {"name": "Zoe", "number": "+44 118 496 0001"}, "/contacts")
        self.ok("/contacts/save", {"name": "alice <b>", "number": "1002"}, "/contacts")
        self.assertEqual(self.contacts(), ["alice <b>|1002", "Zoe|+441184960001"])
        page = b.get("/contacts").body
        self.assertIn("alice &lt;b&gt;", page)
        self.assertNotIn("alice <b>", page)
        self.bad("/contacts/save", {"name": "", "number": "1"}, "/contacts")
        self.bad("/contacts/save", {"name": "Nobody", "number": ""}, "/contacts")
        # change one, found by what it was
        self.assertIn("value='Zoe'", b.get("/contacts?edit=1").body)
        self.ok("/contacts/save", {"was_name": "Zoe", "was_number": "+441184960001", "name": "Zoe W", "number": "1003"}, "/contacts")
        self.assertEqual(self.contacts(), ["alice <b>|1002", "Zoe W|1003"])
        self.bad("/contacts/delete", {"name": "Zoe", "number": "1003"}, "/contacts")
        self.ok("/contacts/delete", {"name": "alice <b>", "number": "1002"}, "/contacts")
        self.assertEqual(self.contacts(), ["Zoe W|1003"])
        # a name with the file's own separator in it cannot break the file
        self.ok("/contacts/save", {"name": "a|b", "number": "5|6"}, "/contacts")
        self.assertIn("a b|5-6", self.contacts())
        self.ok("/contacts/delete", {"name": "a b", "number": "5-6"}, "/contacts")

    def test_51_contacts_import_export(self):
        b = self.b
        csv = 'name,number\r\nBob,1004\r\n"Smith, Carol","+44 20 7946 0000"\r\n"Dee ""D"" Jones",1006\r\nNo number,\r\nZoe W,1003\r\n'
        self.assertEqual(b.upload("/contacts/import", {}, "list.csv", csv).status, 303)
        bad, text = b.flash("/contacts")
        self.assertFalse(bad)
        self.assertIn("3 contacts added", text)
        self.assertEqual(self.contacts(), ['Bob|1004', 'Dee "D" Jones|1006', "Smith, Carol|+442079460000", "Zoe W|1003"])
        vcf = ("BEGIN:VCARD\r\nVERSION:3.0\r\nN:Fox;Erin;;;\r\nFN:Erin Fox\r\nTEL;TYPE=CELL:07700 900123\r\nitem1.TEL;type=WORK:1010\r\nEND:VCARD\r\n"
               "BEGIN:VCARD\r\nVERSION:2.1\r\nN:Gray;Finn\r\nTEL:1011\r\nEND:VCARD\r\n")
        self.assertEqual(b.upload("/contacts/import", {}, "phone.vcf", vcf).status, 303)
        self.assertIn("3 contacts added", b.flash("/contacts")[1])
        self.assertIn("Erin Fox|07700900123", self.contacts())
        self.assertIn("Erin Fox|1010", self.contacts())
        self.assertIn("Gray Finn|1011", self.contacts())
        # out as CSV and vCard, and each back in again gives the same list
        before = self.contacts()
        r = b.get("/contacts/export.csv")
        self.assertIn("attachment", r.getheader("Content-Disposition"))
        self.assertIn('"Smith, Carol","+442079460000"', r.body)
        out_csv, out_vcf = r.body, b.get("/contacts/export.vcf").body
        self.assertIn("FN:Smith\\, Carol", out_vcf)
        for name, text in (("back.csv", out_csv), ("back.vcf", out_vcf)):
            self.assertEqual(b.upload("/contacts/import", {"replace": "yes"}, name, text).status, 303)
            self.assertFalse(b.flash("/contacts")[0])
            self.assertEqual(self.contacts(), before)
        # not a list at all
        self.assertEqual(b.upload("/contacts/import", {}, "junk.bin", "\x7fELF junk").status, 303)
        self.assertTrue(b.flash("/contacts")[0])
        self.assertEqual(self.contacts(), before)

    def test_60_history(self):
        b, p = self.b, self.p
        now = int(time.time())
        with open(os.path.join(p.state, "history"), "w") as f:
            f.write("seen %d\n%d|o|65|1003|Zoe|1\n%d|m|0|07000000000|<i>Spam</i>|1\n%d|i|5|1004||1\n" % (now - 50, now - 300, now - 200, now - 100))
        page = b.get("/history").body
        self.assertIn("3 calls", page)
        self.assertIn("Zoe W", page)            # the contact list's name for the number, as on the phone
        self.assertIn("&lt;i&gt;Spam&lt;/i&gt;", page)
        self.assertIn("1:05", page)
        self.assertLess(page.index("1004"), page.index("07000000000"))   # newest first
        self.bad("/history/clear", {}, "/history")
        self.ok("/history/clear", {"sure": "yes"}, "/history")
        self.assertEqual(read(os.path.join(p.state, "history")), "seen %d\n" % (now - 50))
        self.assertIn("No calls", b.get("/history").body)

    def test_70_apps(self):
        b, p = self.b, self.p
        page = b.get("/apps").body
        self.assertIn("<h2>Clock</h2>", page)
        self.assertIn("Built in", page)
        self.assertIn("<h2>Demo</h2>", page)
        self.assertIn("Needs newer firmware", page)        # the feed's "future" app
        self.assertNotIn("value='future'", page)           # and no button for it
        self.assertIn("installed", self.ok("/apps/install", {"id": "demo"}, "/apps"))
        self.assertTrue(os.path.exists(os.path.join(p.installed, "demo", "manifest.json")))
        self.bad("/apps/install", {"id": "broken"}, "/apps")      # its checksum is wrong
        self.assertFalse(os.path.exists(os.path.join(p.installed, "broken")))
        self.assertEqual(b.post("/apps/install", {"id": "../../etc"}).status, 303)
        # its settings, from its manifest
        page = b.get("/apps").body
        self.assertIn("name='s_title' value='Demo'", page)
        self.ok("/apps/settings", {"id": "demo", "s_title": "Kitchen", "s_pin": "app-pin-4711"}, "/apps")
        self.assertEqual(p.get("apps/demo", "title"), "Kitchen")
        self.assertEqual(p.get("apps/demo", "seconds"), "false")     # an unticked box
        self.assertEqual(p.get("apps/demo", "pin"), "app-pin-4711")
        page = b.get("/apps").body
        self.assertIn("value='Kitchen'", page)
        self.assertNotIn("4711", page)
        self.ok("/apps/settings", {"id": "demo", "s_title": "Kitchen", "s_seconds": "true", "s_pin": ""}, "/apps")
        self.assertEqual(p.get("apps/demo", "pin"), "app-pin-4711")
        self.assertEqual(p.get("apps/demo", "seconds"), "true")
        # an update from the second feed keeps them
        self.bad("/apps/feed", {"url": "http://plain.example/index.json"}, "/apps")
        self.ok("/apps/feed", {"url": "file://" + os.path.join(SHELL_BUILD, "feeds", "2", "index.json")}, "/apps")
        self.assertIn("pill warn'>Update<", b.get("/apps").body)
        self.ok("/apps/update", {"id": "demo"}, "/apps")
        self.assertIn("Installed 1.1.0", b.get("/apps").body)
        self.assertEqual(p.get("apps/demo", "title"), "Kitchen")
        self.ok("/apps/settings", {"id": "demo", "defaults": "yes"}, "/apps")
        self.assertIsNone(p.get("apps/demo", "title"))
        self.ok("/apps/settings", {"id": "demo", "s_title": "Hall", "s_pin": "app-pin-4711"}, "/apps")
        # the clock is part of the firmware
        self.bad("/apps/remove", {"id": "clock"}, "/apps")
        self.ok("/apps/remove", {"id": "demo"}, "/apps")
        self.assertFalse(os.path.exists(os.path.join(p.installed, "demo")))
        self.assertIsNone(p.get("apps/demo", "title"))
        self.assertNotIn("4711", p.logtext())

    def keys(self):
        path = os.path.join(self.p.keep, "ssh", "authorized_keys")
        return read(path).splitlines() if os.path.exists(path) else []

    def test_80_access(self):
        b, p = self.b, self.p
        with open(os.path.join(p.access, "status"), "w") as f:
            f.write("dev=no\nssh=off\nhost_key=SHA256:abcDEF123 (ed25519)\ndoor=closed\nkeys=0\n")
        page = b.get("/access").body
        self.assertIn("SHA256:abcDEF123", page)
        self.assertIn(">Turn on<", page)
        before = len(p.applied_list())
        self.ok("/access/ssh", {"to": "on"}, "/access")
        self.assertEqual(p.get("access", "ssh"), "on")
        self.assertEqual(p.applied_list()[before:], ["access"])
        self.assertIn(">Turn off<", b.get("/access").body)
        # a key pasted in, with the mess a paste brings
        self.ok("/access/key-add", {"keys": "\r\n  " + KEY1 + "  \r\n\r\n"}, "/access")
        self.assertEqual(self.keys(), [KEY1])
        self.assertEqual(os.stat(os.path.join(p.keep, "ssh", "authorized_keys")).st_mode & 0o777, 0o600)
        self.assertIn("access-status", p.applied_list())
        page = b.get("/access").body
        self.assertIn("tom@desk", page)
        # again, a private key, a line with options in front, and junk: all refused
        self.bad("/access/key-add", {"keys": KEY1}, "/access")
        self.bad("/access/key-add", {"keys": "-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXktdjEAAAAA\n-----END OPENSSH PRIVATE KEY-----"}, "/access")
        self.bad("/access/key-add", {"keys": 'command="/bin/sh" ' + KEY2}, "/access")
        self.bad("/access/key-add", {"keys": "ssh-ed25519 notbase64!! x"}, "/access")
        self.bad("/access/key-add", {"keys": "hello"}, "/access")
        self.assertEqual(self.keys(), [KEY1])
        self.ok("/access/key-add", {"keys": KEY2 + "\n# a comment\n"}, "/access")
        self.assertEqual(self.keys(), [KEY1, KEY2])
        self.ok("/access/key-remove", {"key": KEY1.split()[1]}, "/access")
        self.assertEqual(self.keys(), [KEY2])
        self.bad("/access/key-remove", {"key": KEY1.split()[1]}, "/access")
        self.bad("/access/keys-forget", {}, "/access")
        self.assertEqual(self.keys(), [KEY2])
        self.ok("/access/keys-forget", {"sure": "yes"}, "/access")
        self.assertEqual(self.keys(), [])
        self.assertIn("access-forget-keys", p.applied_list())
        self.ok("/access/ssh", {"to": "off"}, "/access")
        self.assertEqual(p.get("access", "ssh"), "off")
        # a key added while SSH is off says so. Not on a development build, where SSH
        # is on whatever the setting says (the real phone showed "SSH is off" there)
        self.assertIn("SSH is off", self.ok("/access/key-add", {"keys": KEY1}, "/access"))
        self.assertIn("SSH off", b.get("/").body)
        with open(os.path.join(p.access, "status"), "w") as f:
            f.write("dev=yes\nssh=on\nhost_key=SHA256:abcDEF123 (ed25519)\ndoor=closed\nkeys=1\n")
        self.assertNotIn("SSH is off", self.ok("/access/key-add", {"keys": KEY2}, "/access"))
        self.assertIn("SSH on", b.get("/").body)
        with open(os.path.join(p.access, "status"), "w") as f:
            f.write("dev=no\nssh=off\nhost_key=SHA256:abcDEF123 (ed25519)\ndoor=closed\nkeys=2\n")
        self.ok("/access/keys-forget", {"sure": "yes"}, "/access")
        # a second browser, signed out from the first
        other = Browser(p).pair()
        self.assertEqual(p.status()["paired"], "2")
        self.assertIn("2 browsers paired", b.get("/access").body)
        self.ok("/access/browsers", {}, "/access")
        self.assertEqual(p.status()["paired"], "1")
        self.assertEqual(other.get("/accounts").status, 303)
        self.assertEqual(b.get("/accounts").status, 200)

    def test_90_backup_round_trip(self):
        b, p = self.b, self.p
        secret = p.get("sip", "password")
        self.assertTrue(secret)
        # an app with a password of its own, which only its manifest marks as one; and
        # what is left of an app that is no longer installed
        self.ok("/apps/install", {"id": "demo"}, "/apps")
        self.ok("/apps/settings", {"id": "demo", "s_title": "Hall", "s_pin": "app-pin-4711"}, "/apps")
        p.set("apps/gone", "key", "leftover-0815")
        self.ok("/accounts/save", {"n": "2", "user": "3003", "domain": "two.example", "password": "second-pw", "transport": "udp", "enabled": "on"}, "/accounts")
        contacts = self.contacts()
        want = {(f, k): p.get(f, k) for f, k in (("sip", "user"), ("sip", "domain"), ("sip", "transport"), ("sip2", "user"), ("network", "mode"),
                                                  ("network", "address"), ("system", "timezone"), ("audio", "ringer"), ("screen", "brightness"),
                                                  ("phone", "default_account"), ("feed", "url"), ("apps/demo", "title"))}
        # without passwords
        r = b.post("/backup/download", {})
        self.assertEqual(r.status, 200)
        plain = r.body
        self.assertTrue(plain.startswith("# T48S settings backup, format 1\n"))
        self.assertIn("# passwords: left out", plain)
        self.assertNotIn("SENSITIVE", plain)
        self.assertNotIn("SENSITIVE", r.getheader("Content-Disposition"))
        self.assertNotIn("password=", plain)
        self.assertNotIn(secret, plain)
        self.assertNotIn("second-pw", plain)
        self.assertNotIn("app-pin-4711", plain)
        self.assertIn("[config apps/demo]\n", plain)
        self.assertNotIn("leftover-0815", plain)
        self.assertIn("[config sip]\nuser=1001\n", plain)
        self.assertIn("[contacts]\n", plain)
        self.assertTrue(plain.endswith("[end]\n"))
        self.assertNotIn("[config access]", plain)
        # with them: marked
        r = b.post("/backup/download", {"passwords": "yes"})
        full = r.body
        self.assertIn("# passwords: included", full)
        self.assertIn("# SENSITIVE: contains passwords", full)
        self.assertIn("SENSITIVE", r.getheader("Content-Disposition"))
        self.assertIn("password=" + secret, full)
        self.assertIn("password=second-pw", full)
        self.assertIn("pin=app-pin-4711", full)
        self.assertIn("leftover-0815", full)
        self.assertNotIn(secret, p.logtext())
        self.assertIn("backup downloaded, passwords included", p.logtext())

        def wreck():
            self.ok("/accounts/save", {"n": "1", "user": "9999", "domain": "wrong.example", "password": "changed-pw", "transport": "tcp", "enabled": "on"}, "/accounts")
            self.ok("/accounts/remove", {"n": "2", "sure": "yes"}, "/accounts")
            self.ok("/time", {"zone": "Asia/Tokyo"}, "/time")
            self.ok("/sound", {"ringer": "15", "handset": "15", "speaker": "15", "headset": "15", "apps": "15"}, "/sound")
            self.ok("/network", {"mode": "static", "address": "10.9.9.9", "netmask": "255.0.0.0"}, "/network")
            self.ok("/contacts/save", {"name": "Intruder", "number": "666"}, "/contacts")
            p.set("apps/demo", "title", "Wrecked")
            p.set("apps/demo", "pin", "other-pin")

        # the box must be ticked
        wreck()
        self.assertEqual(b.upload("/backup/restore", {}, "b.txt", full).status, 303)
        self.assertTrue(b.flash("/backup")[0])
        self.assertEqual(p.get("sip", "user"), "9999")
        # the full file back: everything as it was, passwords too
        self.assertEqual(b.upload("/backup/restore", {"sure": "yes"}, "b.txt", full).status, 303)
        bad, text = b.flash("/backup")
        self.assertFalse(bad, text)
        for (f, k), v in want.items():
            self.assertEqual(p.get(f, k), v, (f, k))
        self.assertEqual(p.get("sip", "password"), secret)
        self.assertEqual(p.get("sip2", "password"), "second-pw")
        self.assertEqual(p.get("apps/demo", "pin"), "app-pin-4711")
        self.assertEqual(self.contacts(), contacts)
        self.assertEqual(os.stat(os.path.join(p.config, "sip")).st_mode & 0o777, 0o600)
        # the file without passwords: everything as it was, and the passwords the phone has now are kept
        wreck()
        self.assertEqual(b.upload("/backup/restore", {"sure": "yes"}, "b.txt", plain.replace("\n", "\r\n")).status, 303)
        self.assertFalse(b.flash("/backup")[0])
        for (f, k), v in want.items():
            self.assertEqual(p.get(f, k), v, (f, k))
        self.assertEqual(p.get("sip", "password"), "changed-pw")
        self.assertEqual(p.get("apps/demo", "pin"), "other-pin")
        self.assertEqual(self.contacts(), contacts)
        # files that are not backups, or not whole, or reach for things a backup may not hold: nothing changes
        for what, text in (("not one", "hello\n"),
                           ("cut short", full[:len(full) // 2]),
                           ("no end", full.replace("[end]\n", "")),
                           ("ssh", full.replace("[contacts]", "[config access]\nssh=on\n[contacts]")),
                           ("climbs out", full.replace("[contacts]", "[config ../../etc/passwd]\nroot=x\n[contacts]")),
                           ("not a setting", full.replace("user=1001", "user=1001\nrm -rf /")),
                           ("after the end", full + "user=late\n")):
            wreck_user = "tamper-check"
            p.set("sip", "label", wreck_user)
            time.sleep(0)
            self.assertEqual(b.upload("/backup/restore", {"sure": "yes"}, "b.txt", text).status, 303, what)
            bad, msg = b.flash("/backup")
            self.assertTrue(bad, what)
            self.assertIn("Not restored", msg, what)
            self.assertEqual(p.get("sip", "label"), wreck_user, what)
        self.assertEqual(p.get("access", "ssh"), "off")
        self.assertFalse(os.path.exists(os.path.join(p.dir, "etc")))

    def test_95_pages_and_log(self):
        b, p = self.b, self.p
        for path in ("/", "/accounts", "/network", "/time", "/sound", "/contacts", "/history", "/apps", "/access", "/backup", "/update"):
            r = b.get(path)
            self.assertEqual(r.status, 200, path)
            self.assertIn("Plain HTTP", r.body, path)
            self.assertIn("name='csrf'", r.body, path)
            for secret in ("s3cret", "second-pw", "changed-pw", "4711"):
                self.assertNotIn(secret, r.body, path)
        log = p.logtext()
        for secret in ("s3cret", "second-pw", "changed-pw", "4711", "hunter2", b.cookies["t48s"], b.csrf, "AAAAC3Nza"):
            self.assertNotIn(secret, log)
        self.assertEqual(b.request("HEAD", "/accounts").body, "")

    def test_99_turned_off_from_the_page(self):
        b, p = self.b, self.p
        self.bad("/access/web-off", {}, "/access")
        self.assertEqual(p.get("access", "web"), "on")
        r = b.post("/access/web-off", {"sure": "yes"})
        self.assertIn("Web setup is off", r.body)
        self.assertEqual(p.get("access", "web"), "off")
        self.assertEqual(p.proc.wait(5), 0)
        time.sleep(1.3)
        self.assertIn("access", p.applied_list()[-1:])


class Update(unittest.TestCase):
    """The Update page, with a stand-in for t48-update (tests/update.sh)."""

    def setUp(self):
        self.p = Phone().start()
        self.b = Browser(self.p).pair()

    def tearDown(self):
        self.p.stop()

    def fake(self, name, text=None):
        path = os.path.join(self.p.fake, name)
        if text is None:
            if os.path.exists(path):
                os.remove(path)
        else:
            with open(path + ".new", "w") as f:
                f.write(text)
            os.rename(path + ".new", path)

    def calls(self, without_info=True):
        path = os.path.join(self.p.fake, "calls")
        c = read(path).splitlines() if os.path.exists(path) else []
        return [x for x in c if x != "info"] if without_info else c

    def fact(self, body, name):
        m = re.search(r"<dt>%s</dt><dd>(.*?)</dd>" % name, body)
        return re.sub(r"<[^>]+>", "", m.group(1)) if m else None

    def page(self, until=None, tries=80):
        """the page; with until, asked for again as a browser would until it says so"""
        for _ in range(tries):
            r = self.b.get("/update")
            self.assertEqual(r.status, 200)
            if until is None or until(r):
                return r
            time.sleep(0.1)
        self.fail("the page never got there: " + r.body)

    def post(self, path, fields=None):
        r = self.b.post(path, fields)
        self.assertEqual((r.status, r.getheader("Location")), (303, "/update"), r.body)

    def flash(self, r):
        m = re.search(r"<p class='flash( bad)?' role='status'>(.*?)</p>", r.body)
        return (bool(m.group(1)), m.group(2)) if m else (None, "")

    def test_before_a_check(self):
        r = self.page()
        self.assertEqual(self.fact(r.body, "Version"), "v0.2.0, slot A")
        self.assertEqual(self.fact(r.body, "Latest"), "Not checked")
        self.assertIsNone(self.fact(r.body, "Status"))
        self.assertEqual(self.fact(r.body, "Last result"), "None")
        self.assertIn("<option value='stable' selected>Stable</option>", r.body)
        self.assertIn("Check now", r.body)
        for absent in ("/update/install", "/update/restart", "/update/kernel"):
            self.assertNotIn(absent, r.body)
        self.assertIsNone(r.getheader("Refresh"))
        self.assertIn("href='/update' class='here'>Update</a>", r.body)
        self.assertIn("href='/update'", self.b.get("/").body)
        # looking never checks
        self.assertEqual(self.calls(), [])
        self.assertIn("info", self.calls(False))
        # and a GET starts nothing
        self.assertEqual(self.b.get("/update/check").status, 404)
        self.assertEqual(self.calls(), [])

    def test_check_then_update_available(self):
        self.fake("offer", "latest=v0.3.0\nupdate=yes\nkernel=same\n")
        self.fake("hold-checking", "")
        self.post("/update/check")
        r = self.page(lambda r: self.fact(r.body, "Status") == "Checking")
        # it asks for itself again, and offers nothing meanwhile
        self.assertIsNotNone(r.getheader("Refresh"))
        self.assertNotIn("<form method='post' action='/update/", r.body)
        self.assertEqual(self.fact(r.body, "Channel"), "Stable")
        # a second run is refused while one is going
        self.post("/update/check")
        self.assertEqual(self.flash(self.page()), (True, "Update already running."))
        self.fake("hold-checking")
        r = self.page(lambda r: self.fact(r.body, "Status") == "Update available")
        self.assertRegex(self.fact(r.body, "Latest"), r"^v0\.3\.0, checked \d{1,2} [A-Z][a-z]{2} \d\d:\d\d$")
        self.assertIn("action='/update/install'", r.body)
        self.assertNotIn("/update/kernel", r.body)
        self.assertNotIn("/update/restart", r.body)
        self.assertEqual(self.calls(), ["check"])
        self.assertIsNone(self.page().getheader("Refresh"))
        # up to date: nothing to install
        self.fake("offer", "latest=v0.2.0\nupdate=no\nkernel=same\n")
        self.post("/update/check")
        r = self.page(lambda r: self.fact(r.body, "Status") == "Up to date")
        self.assertNotIn("/update/install", r.body)
        self.post("/update/install")
        self.assertEqual(self.flash(self.page()), (True, "No update to install."))
        self.assertEqual(self.calls(), ["check", "check"])

    def test_install_then_restart(self):
        self.fake("offer", "latest=v0.3.0\nupdate=yes\nkernel=same\n")
        # not before a check has found one
        self.post("/update/install")
        self.assertEqual(self.flash(self.page()), (True, "No update to install."))
        self.post("/update/check")
        self.page(lambda r: self.fact(r.body, "Status") == "Update available")
        self.fake("hold-downloading", "")
        self.fake("hold-writing", "")
        t = time.time()
        self.post("/update/install")
        self.assertLess(time.time() - t, 2)   # started, not waited for
        r = self.page(lambda r: self.fact(r.body, "Status") == "Downloading")
        self.assertEqual(r.getheader("Refresh"), "3")
        self.fake("hold-downloading")
        r = self.page(lambda r: self.fact(r.body, "Status") == "Writing")
        self.assertIsNotNone(r.getheader("Refresh"))
        self.assertNotIn("<form method='post' action='/update/", r.body)
        self.fake("hold-writing")
        r = self.page(lambda r: self.fact(r.body, "Status") == "Restart to finish")
        self.assertIn("action='/update/restart'", r.body)
        self.assertNotIn("/update/install", r.body)
        self.assertIsNone(self.page().getheader("Refresh"))
        # the restart wants its tick
        self.post("/update/restart")
        self.assertEqual(self.flash(self.page()), (True, "Tick Confirm first."))
        self.assertFalse(os.path.exists(os.path.join(self.p.fake, "restarted")))
        self.post("/update/restart", {"sure": "yes"})
        self.assertEqual(self.flash(self.page()), (False, "Restarting."))
        for _ in range(40):
            if os.path.exists(os.path.join(self.p.fake, "restarted")):
                break
            time.sleep(0.1)
        self.assertEqual(read(os.path.join(self.p.fake, "restarted")), "restart\n")
        self.assertEqual(self.calls(), ["check", "install"])
        # what the updater wrote down after the restart
        self.fake("state", "state=idle\nmessage=\n")
        self.fake("result", "result=v0.3.0 installed\nresult_time=1795000000\n")
        r = self.page()
        self.assertRegex(self.fact(r.body, "Last result"), r"^v0\.3\.0 installed, \d{1,2} [A-Z][a-z]{2} \d\d:\d\d$")
        # and no restart without an update waiting
        self.post("/update/restart", {"sure": "yes"})
        self.assertEqual(self.flash(self.page()), (True, "No update to restart for."))
        self.assertEqual(read(os.path.join(self.p.fake, "restarted")), "restart\n")

    def test_a_failed_check(self):
        self.fake("fail", "No release found <b>")
        self.post("/update/check")
        r = self.page(lambda r: self.fact(r.body, "Status") not in (None, "Checking"))
        # the cause as the updater worded it, and nothing of it taken as HTML
        self.assertEqual(self.fact(r.body, "Status"), "No release found &lt;b&gt;")
        self.assertIn("<span class='pill bad'>No release found &lt;b&gt;</span>", r.body)
        self.assertEqual(self.fact(r.body, "Latest"), "Not checked")
        self.assertRegex(self.fact(r.body, "Last result"), r"^No release found &lt;b&gt;, ")
        self.assertIn("Check now", r.body)
        self.assertNotIn("/update/install", r.body)

    def test_channel(self):
        self.fake("offer", "latest=v0.3.0\nupdate=yes\nkernel=same\n")
        self.post("/update/check")
        self.page(lambda r: self.fact(r.body, "Status") == "Update available")
        self.post("/update/channel", {"channel": "testing", "url": ""})
        r = self.page()
        self.assertEqual(self.flash(r), (False, "Channel saved."))
        self.assertIn("<option value='testing' selected>Testing</option>", r.body)
        # what the other channel offered is no longer said
        self.assertEqual(self.fact(r.body, "Latest"), "Not checked")
        self.post("/update/channel", {"channel": "custom", "url": " https://builds.example.org/t48s/ "})
        r = self.page()
        self.assertEqual(self.flash(r), (False, "Channel saved."))
        self.assertIn("<option value='custom' selected>Custom</option>", r.body)
        self.assertIn("value='https://builds.example.org/t48s/'", r.body)
        self.assertEqual(read(os.path.join(self.p.fake, "channel")), "https://builds.example.org/t48s/\n")
        # refused here: not a URL at all, nothing, an option for the updater
        for url in ("builds.example.org", "", "--help", "ftp://builds.example.org/", "https://a b"):
            self.post("/update/channel", {"channel": "custom", "url": url})
            self.assertEqual(self.flash(self.page()), (True, "Not saved: a custom channel is an https:// or http:// URL."), url)
        self.post("/update/channel", {"channel": "nightly"})
        self.assertIs(self.flash(self.page())[0], True)
        # refused by the updater, with its reason
        self.post("/update/channel", {"channel": "custom", "url": "https://builds.example.org/?a=$(reboot)"})
        self.assertEqual(self.flash(self.page()), (True, "Not saved: Not a URL."))
        self.assertEqual(read(os.path.join(self.p.fake, "channel")), "https://builds.example.org/t48s/\n")
        self.post("/update/channel", {"channel": "stable", "url": "https://builds.example.org/t48s/"})
        self.assertIn("<option value='stable' selected>Stable</option>", self.page().body)
        self.assertEqual(self.calls(), ["check", "channel testing", "channel https://builds.example.org/t48s/",
                                        "channel https://builds.example.org/?a=$(reboot)", "channel stable"])
        self.assertNotIn("builds.example.org", self.p.logtext())

    def test_kernel_differs(self):
        self.fake("offer", "latest=v0.3.0\nupdate=no\nkernel=differs\n")
        # not offered, and not started, before a check says so
        self.assertNotIn("/update/kernel", self.page().body)
        self.post("/update/kernel", {"sure": "yes"})
        self.assertEqual(self.flash(self.page()), (True, "No kernel to install."))
        self.post("/update/check")
        r = self.page(lambda r: self.fact(r.body, "Status") == "Up to date")
        m = re.search(r"<form method='post' action='/update/kernel'.*?</form>", r.body)
        self.assertIn("name='sure' value='yes' required> Confirm", m.group(0))
        self.assertIn(">Install kernel</button>", m.group(0))
        self.assertIn("A failed kernel needs recovery mode.", m.group(0))
        self.post("/update/kernel")
        self.assertEqual(self.flash(self.page()), (True, "Tick Confirm first."))
        self.assertEqual(self.calls(), ["check"])
        self.post("/update/kernel", {"sure": "yes"})
        r = self.page(lambda r: self.fact(r.body, "Status") == "Restart to finish")
        self.assertEqual(self.calls(), ["check", "kernel"])
        self.assertRegex(self.fact(r.body, "Last result"), r"^Kernel written, ")
        self.assertIn("action='/update/restart'", r.body)
        self.assertNotIn("/update/kernel", r.body)

    def test_not_installed(self):
        self.fake("installed", "no")
        r = self.page()
        self.assertEqual(self.fact(r.body, "Version"), "v0.2.0")
        self.assertIn("Not installed", r.body)
        self.assertNotIn("action='/update/", r.body)
        self.assertIsNone(self.fact(r.body, "Latest"))
        for path in ("/update/check", "/update/install", "/update/kernel"):
            self.post(path, {"sure": "yes"})
            self.assertEqual(self.flash(self.page()), (True, "Not installed."), path)
        self.assertEqual(self.calls(), [])

    def test_forms_need_the_token(self):
        self.fake("offer", "latest=v0.3.0\nupdate=yes\nkernel=differs\n")
        for path in ("/update/check", "/update/install", "/update/kernel", "/update/restart", "/update/channel"):
            fields = {"sure": "yes", "channel": "testing"}
            self.assertEqual(self.b.post(path, fields, csrf=False).status, 403, path)
            self.assertEqual(self.b.post(path, fields, headers={"Origin": "http://evil.example"}).status, 403, path)
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    if not os.path.exists(WEB):
        sys.exit("no %s: run make first" % WEB)
    unittest.main(verbosity=1)
