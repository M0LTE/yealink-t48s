#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A browser for the test with the phone's simulator (phone.script): one step a
run, its cookies kept in a file between runs.

  browser.py JAR URL start             ask the phone to show a code
  browser.py JAR URL finish STATUS     type the code (read from the status file the screen shows it from)
  browser.py JAR URL finish-refused STATUS   the same, and expect to be turned away
  browser.py JAR URL post PATH k=v...  send a form, as a paired browser; fails unless the phone says it worked
  browser.py JAR URL expect PATH TEXT  the page must hold TEXT
  browser.py JAR URL absent PATH TEXT  the page must not hold TEXT
"""
import http.cookiejar, re, sys, urllib.error, urllib.parse, urllib.request

jar_path, base, cmd, args = sys.argv[1], sys.argv[2].rstrip("/"), sys.argv[3], sys.argv[4:]
jar = http.cookiejar.MozillaCookieJar(jar_path)
try:
    jar.load(ignore_discard=True)
except FileNotFoundError:
    pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar), NoRedirect)


def go(path, fields=None):
    data = urllib.parse.urlencode(fields).encode() if fields is not None else None
    try:
        r = opener.open(urllib.request.Request(base + path, data=data), timeout=120)
    except urllib.error.HTTPError as e:
        r = e
    body = r.read().decode("utf-8", "replace")
    jar.save(ignore_discard=True)
    return r.status if hasattr(r, "status") else r.code, r.headers.get("Location"), body


def code_on_screen(status_file):
    for line in open(status_file).read().splitlines():
        if line.startswith("pair_code="):
            return line.split("=", 1)[1]
    sys.exit("no code is showing")


if cmd == "start":
    st, loc, _ = go("/pair/start", {})
    if (st, loc) != (303, "/pair"):
        sys.exit("asked for a code, got %s %s" % (st, loc))
elif cmd in ("finish", "finish-refused"):
    code = code_on_screen(args[0]) if cmd == "finish" else args[1]
    st, loc, _ = go("/pair/finish", {"code": code})
    want = "/" if cmd == "finish" else "/?e=gone"
    if (st, loc) != (303, want):
        sys.exit("typed the code, got %s %s, wanted %s" % (st, loc, want))
elif cmd == "post":
    path = args[0]
    fields = dict(a.split("=", 1) for a in args[1:])
    _, _, page = go("/")
    m = re.search(r"name='csrf' value='([0-9a-f]+)'", page)
    if not m:
        sys.exit("not paired")
    fields["csrf"] = m.group(1)
    st, loc, _ = go(path, fields)
    if st != 303:
        sys.exit("%s: got %s" % (path, st))
    _, _, page = go(loc.split("#")[0])
    m = re.search(r"<p class='flash( bad)?' role='status'>(.*?)</p>", page)
    if not m or m.group(1):
        sys.exit("%s: %s" % (path, m.group(2) if m else "the phone said nothing"))
    print("browser: %s: %s" % (path, m.group(2)), file=sys.stderr)
elif cmd in ("expect", "absent"):
    st, loc, page = go(args[0])
    if (args[1] in page) != (cmd == "expect"):
        sys.exit("%s: %r is %s the page (%s %s)" % (args[0], args[1], "not in" if cmd == "expect" else "in", st, loc))
else:
    sys.exit(__doc__)
