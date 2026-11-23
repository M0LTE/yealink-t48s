#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Two small feeds on disk for the app manager's tests (tests/05-apps.script),
# made with the SDK's own packer from the example clock:
#   feeds/1: demo 1.0.0, "future" (written for an API level this shell does not
#            speak) and "broken" (its package does not match its checksum)
#   feeds/2: demo 1.1.0
# and feeds/evil.t48app, a package that tries to write outside its folder.
set -e
O=$1
HERE=$(cd "$(dirname "$0")" && pwd)
PACK="python3 $HERE/../../sdk/t48-pack"
rm -rf "$O/feeds"
mkdir -p "$O/feeds/1" "$O/feeds/2" "$O/feeds/src"
app() {  # id name version api feed
    d="$O/feeds/src/$1-$3/$1"
    mkdir -p "$d"
    cp "$O/apps/clock/clock" "$O/apps/clock/icon.ppm" "$d/"
    cat > "$d/manifest.json" <<JSON
{ "id": "$1", "name": "$2", "version": "$3", "api": $4, "exec": "clock", "icon": "icon.ppm",
  "description": "A test app: the example clock under another name",
  "permissions": ["network"],
  "settings": [ { "key": "title", "label": "Title", "type": "text", "default": "$2" },
                { "key": "seconds", "label": "Show seconds", "type": "bool", "default": true },
                { "key": "pin", "label": "A secret", "type": "password", "default": "" } ] }
JSON
    $PACK "$d" "$O/feeds/$5" >/dev/null 2>&1
}
app demo Demo 1.0.0 1 1
app future Future 3.0.0 2 1
app broken Broken 1.0.0 1 1
app demo Demo 1.1.0 1 2
$PACK --index "$O/feeds/1" >/dev/null
$PACK --index "$O/feeds/2" >/dev/null
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
