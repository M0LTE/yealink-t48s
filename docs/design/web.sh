#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# web.sh [DIRECTION...]: the web setup page's pictures for docs/design, taken from the
# real server with Playwright. Run render.sh first: this uses what it built.
#   NODE_PATH=.../node_modules docs/design/web.sh
# Switchboard is webconfig's own style sheet and fonts; the other two have theirs here.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PORT=${WEB_PORT:-8439}
[ $# -gt 0 ] || set -- switchboard directory twotone
for d in "$@"; do
    O=build-$d
    SB=$ROOT/shell/$O
    if [ "$d" = switchboard ]; then
        make -s -C "$ROOT/webconfig" O=$O
    else
        make -s -C "$ROOT/webconfig" O=$O STYLE="$HERE/$d/web.css" FONT_REGULAR="$HERE/$d/regular.woff2" FONT_BOLD="$HERE/$d/bold.woff2"
    fi
    make -s -C "$ROOT/shell" O=$O $O/feeds >/dev/null
    T=$(mktemp -d)
    cfg() { "$SB/t48-config" --config "$T/config" set "$@"; }
    mkdir -p "$T/config" "$T/phone" "$T/access" "$T/keep/ssh" "$T/installed"
    cfg access web on; cfg access ssh on
    cfg sip user 1001; cfg sip domain pbx.example.org; cfg sip password not-a-real-one
    cfg sip display_name "Tom M0LTE"; cfg sip label Home; cfg sip voicemail "*97"; cfg sip enabled on
    cfg sip2 user tom; cfg sip2 domain sip.club.example; cfg sip2 password another; cfg sip2 transport tls
    cfg sip2 label Club; cfg sip2 enabled on
    cfg phone default_account 1; cfg system timezone Europe/London
    cfg audio ringer 9; cfg screen brightness 8
    cfg feed url "file://$SB/feeds/1/index.json"
    printf '%s\n' 'Alice Example|1002' 'Bob Example|+441184960123' 'Club bar|2001' 'Front door|*8' 'Mum|01184960456' > "$T/phone/contacts"
    now=$(date +%s)
    printf 'seen %s\n%s|o|312|1002|Alice Example|1\n%s|i|64|2001|Club bar|2\n%s|m|0|07700900123||1\n' $((now - 100)) $((now - 90000)) $((now - 80000)) $((now - 20000)) > "$T/phone/history"
    echo "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOeStCLCiE0njMGEkRNUHaNmd+dNxoLC9NECKxV/wLQ9 tom@desk" > "$T/keep/ssh/authorized_keys"
    printf 'dev=no\nssh=on\nhost_key=SHA256:6Ru8gBwrS00sNCkM/wL0/lOtLVJuGwKvLVdhZR08WAg (ed25519)\ndoor=closed\nkey=ssh-ed25519 tom@desk\nkeys=1\n' > "$T/access/status"
    "$SB/t48-app" --apps "$T/installed:$SB/apps" --config "$T/config" --data "$T/appdata" install demo >/dev/null
    "$ROOT/webconfig/$O/t48-web" --listen "127.0.0.1:$PORT" --config "$T/config" --state "$T/phone" --apps "$T/installed:$SB/apps" --data "$T/appdata" \
        --apply /bin/true --app "$SB/t48-app" --access "$T/access" --keys "$T/keep" --run "$T/run" 2>"$T/log" &
    PID=$!
    sleep 0.5
    node "$HERE/web.js" "http://127.0.0.1:$PORT" "$T/run/status" "$T/shots" || { kill $PID; cat "$T/log"; exit 1; }
    kill $PID
    python3 - "$T/shots" "$HERE/$d" <<'PY'
import os, sys
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
for name in sorted(os.listdir(src)):
    im = Image.open(os.path.join(src, name)).convert("RGB").quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    im.save(os.path.join(out, name), optimize=True)
PY
    rm -rf "$T"
    echo "$d: $(ls "$HERE/$d" | grep -c '^web-') web pictures"
done
