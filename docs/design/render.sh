#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# render.sh [DIRECTION...]: the phone's screens for docs/design, drawn by the shell's own
# simulator (the real code, the real fonts). Needs python3-pil for the font tables.
#   docs/design/render.sh                 all three, into docs/design/<direction>/
# FONTDIR names the folder with the TTF files (default libt48/fonts).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
FONTDIR=${FONTDIR:-$ROOT/libt48/fonts}
[ $# -gt 0 ] || set -- switchboard directory twotone
for d in "$@"; do
    case $d in
    switchboard) n=0; set_=b612 ;;
    directory) n=1; set_=plex ;;
    twotone) n=2; set_=figtree ;;
    *) echo "no such direction: $d"; exit 2 ;;
    esac
    O=build-$d
    mkdir -p "$ROOT/shell/$O"
    python3 "$ROOT/libt48/tools/mkfont.py" "$ROOT/shell/$O/fonts_gen.c" $set_ "$FONTDIR" >/dev/null
    make -s -C "$ROOT/libt48" O=$O FONTS="$ROOT/shell/$O/fonts_gen.c"
    CFLAGS="-O2 -DT48_THEME=$n" make -s -C "$ROOT/shell" O=$O all $O/apps
    W=$ROOT/shell/$O/design
    rm -rf "$W" && mkdir -p "$W/config" "$W/state" "$W/shots"
    cfg() { "$ROOT/shell/$O/t48-config" --config "$W/config" set "$@"; }
    cfg sip user 1001; cfg sip domain pbx.example.org; cfg sip label Home
    cfg system timezone Europe/London
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
