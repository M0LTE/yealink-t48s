#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Two small feeds on disk for the app manager's tests (tests/05-apps.script),
# made with the SDK's own packer from the example clock:
#   feeds/1: demo 1.0.0, "future" (written for an API level this shell does not
#            speak) and "broken" (its package does not match its checksum)
#   feeds/2: demo 1.1.0
#   feeds/3: demo 1.0.0 and memo 1.0.0; feeds/4: both at 1.1.0 (Update all on the web page)
#   feeds/5: apps in categories (issue 30, tests/23-app-categories.script): waves (Radio),
#            blocks (Games), meter (Tools), notes (none in its manifest, Tools in the
#            feed) and odd (a category this firmware does not know: General)
#   feeds/6 to 9: more than one feed (issue 81, tests/24-feeds.script). A club's feed,
#            6 then 8: demo 1.0.0 then 1.2.0, and kite (in feed 8 with a description that
#            is HTML: the web page must show it as text); another, 7 then 9: demo 1.1.0
#            (in both feeds, newer here), kite 1.2.0, and lark 1.0.0 then 1.1.0
#   feeds/hostile: an added feed that offers clock (built in) 9.9.9 and memo 2.0.0, for
#            tests/feeds-trust.sh: neither may replace what the official feed installed
#   feeds/official: no apps, the official feed in the tests (T48_OFFICIAL_FEED), which
#            never reach the internet; feeds/bad: an index that is not JSON
# None of feeds 1 to 4 has categories: as an old feed, everything is General.
# and feeds/evil.t48app, a package that tries to write outside its folder.
set -e
O=$1
HERE=$(cd "$(dirname "$0")" && pwd)
PACK="python3 $HERE/../../sdk/t48-pack"
rm -rf "$O/feeds"
mkdir -p "$O/feeds/1" "$O/feeds/2" "$O/feeds/3" "$O/feeds/4" "$O/feeds/5" "$O/feeds/6" "$O/feeds/7" "$O/feeds/8" "$O/feeds/9" \
    "$O/feeds/official" "$O/feeds/bad" "$O/feeds/hostile" "$O/feeds/src"
app() {  # id name version api feed [category]
    d="$O/feeds/src/$5/$1-$3/$1"
    mkdir -p "$d"
    cat=""
    [ -z "$6" ] || cat="\"category\": \"$6\","
    cp "$O/apps/clock/clock" "$O/apps/clock/icon.ppm" "$d/"
    cat > "$d/manifest.json" <<JSON
{ "id": "$1", "name": "$2", "version": "$3", "api": $4, "exec": "clock", "icon": "icon.ppm",
  "description": "A test app: the example clock under another name",
  "permissions": ["network"], $cat
  "settings": [ { "key": "title", "label": "Title", "type": "text", "default": "$2" },
                { "key": "seconds", "label": "Show seconds", "type": "bool", "default": true },
                { "key": "pin", "label": "A secret", "type": "password", "default": "" } ] }
JSON
    $PACK "$d" "$O/feeds/$5" >/dev/null 2>&1
}
app demo Demo 1.0.0 1 1
app future Future 3.0.0 9 1
app broken Broken 1.0.0 1 1
app demo Demo 1.1.0 1 2
app demo Demo 1.0.0 1 3
app memo Memo 1.0.0 1 3
app demo Demo 1.1.0 1 4
app memo Memo 1.1.0 1 4
app waves Waves 1.0.0 1 5 Radio
app blocks Blocks 1.0.0 1 5 Games
app meter Meter 1.0.0 1 5 Tools
app notes Notes 1.0.0 1 5
app odd Odd 1.0.0 1 5 Media
app demo Demo 1.0.0 1 6
app kite Kite 1.0.0 1 6 Radio
app demo Demo 1.1.0 1 7
app kite Kite 1.2.0 1 7 Radio
app lark Lark 1.0.0 1 7 Tools
app demo Demo 1.2.0 1 8
app kite Kite 1.0.0 1 8 Radio
app demo Demo 1.1.0 1 9
app kite Kite 1.2.0 1 9 Radio
app lark Lark 1.1.0 1 9 Tools
app clock Clock 9.9.9 1 hostile
app memo Memo 2.0.0 1 hostile
for n in 1 2 3 4 5 6 7 8 9 hostile; do $PACK --index "$O/feeds/$n" >/dev/null; done
echo '{ "feed": 1, "apps": [] }' > "$O/feeds/official/index.json"
echo 'not a feed <html>' > "$O/feeds/bad/index.json"
# feeds 1 to 4 as an old t48-pack wrote them, with no "category"; in feed 5, notes gets
# one only from the feed, and odd one from a newer firmware's list
python3 - "$O/feeds" <<'PY'
import json, sys
for n in "1234":
    p = f"{sys.argv[1]}/{n}/index.json"
    f = json.load(open(p))
    for a in f["apps"]:
        del a["category"]
    json.dump(f, open(p, "w"), indent=2)
p = f"{sys.argv[1]}/5/index.json"
f = json.load(open(p))
for a in f["apps"]:
    a["category"] = {"notes": "Tools", "odd": "Weather"}.get(a["id"], a["category"])
json.dump(f, open(p, "w"), indent=2)
for n in "8":
    p = f"{sys.argv[1]}/{n}/index.json"
    f = json.load(open(p))
    for a in f["apps"]:
        if a["id"] == "kite":
            a["description"] = "Notes <script>alert('lark')</script> & more"
    json.dump(f, open(p, "w"), indent=2)
PY
# same size, one byte different: only the checksum can tell
python3 - "$O/feeds/1/broken-1.0.0.t48app" <<'PY'
import sys
p = sys.argv[1]
b = bytearray(open(p, "rb").read())
b[len(b) // 2] ^= 0x55
open(p, "wb").write(b)
PY
# a package with a file that climbs out of its folder, and a symlink
python3 - "$O/feeds/evil.t48app" "$O/feeds/link.t48app" <<'PY'
import io, sys, tarfile
def make(path, members):
    with tarfile.open(path, "w:gz", format=tarfile.USTAR_FORMAT) as t:
        for name, kind, data in members:
            ti = tarfile.TarInfo(name)
            ti.type = kind
            if kind == tarfile.SYMTYPE:
                ti.linkname = data
                t.addfile(ti)
            else:
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
make(sys.argv[1], [("evil/manifest.json", tarfile.REGTYPE, b"{}"), ("evil/../../escaped", tarfile.REGTYPE, b"out")])
make(sys.argv[2], [("link/manifest.json", tarfile.REGTYPE, b"{}"), ("link/passwd", tarfile.SYMTYPE, "/etc/passwd")])
PY
