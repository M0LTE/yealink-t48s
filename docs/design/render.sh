#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# render.sh [THEME...]: the phone's screens for docs/design, drawn by the shell's own
# simulator (the real code, the real fonts), once in each theme.
#   docs/design/render.sh                 all three, into docs/design/<theme>/
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
O=build
[ $# -gt 0 ] || set -- switchboard directory twotone
make -s -C "$ROOT/shell" O=$O all $O/apps
for d in "$@"; do
    W=$ROOT/shell/$O/design-$d
    rm -rf "$W" && mkdir -p "$W/config" "$W/state" "$W/shots"
    cfg() { "$ROOT/shell/$O/t48-config" --config "$W/config" set "$@"; }
    cfg sip user 1001; cfg sip domain pbx.example.org; cfg sip label Home
    cfg system timezone Europe/London
    cfg screen theme $d
    printf '%s\n' 'Alice Example|1002' 'Bob Example|+441184960123' > "$W/state/contacts"
    python3 "$HERE/fakesip.py" "$W/sip.sock" sip:1001@pbx.example.org &
    FAKE=$!
    sleep 0.3
    (cd "$W" && ../t48-sim --apps ../apps --config config --state state --data appdata --access access --sip sip.sock \
        --script "$HERE/screens.script" 2>sim.log) || { cat "$W/sim.log"; kill $FAKE; exit 1; }
    kill $FAKE
    mkdir -p "$HERE/$d"
    python3 - "$W/shots" "$HERE/$d" <<'PY'
import os, sys
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
for name in sorted(os.listdir(src)):
    im = Image.open(os.path.join(src, name)).convert("RGB").quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    im.save(os.path.join(out, name), optimize=True)
PY
    echo "$d: $(ls "$HERE/$d" | grep -c phone-) phone pictures"
done
