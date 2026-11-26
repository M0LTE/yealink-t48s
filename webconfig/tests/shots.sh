#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# shots.sh BUILD_DIR: pictures of the web page for docs/web.md, into docs/img.
# A made-up phone (accounts, contacts, calls, two apps with updates, a key, do not disturb) is served by the
# real program, and shots.js drives a browser over it with Playwright. If
# Playwright's own browser is not installed, CHROMIUM names one to use.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
O=$(basename "$1")
WEB=$(cd "$HERE/../$O" && pwd)/t48-web
SB=$(cd "$HERE/../../shell/$O" && pwd)
OUT=$(cd "$HERE/../../docs/img" && pwd)
PORT=${WEB_PORT:-8438}
T=$(mktemp -d)
trap 'kill $PID 2>/dev/null; rm -rf "$T"' EXIT
cfg() { "$SB/t48-config" --config "$T/config" set "$@"; }
mkdir -p "$T/config" "$T/phone" "$T/access" "$T/keep/ssh" "$T/installed"
cfg access web on
cfg access ssh on
cfg sip user 1001; cfg sip domain pbx.example.org; cfg sip password not-a-real-one
cfg sip display_name "Tom M0LTE"; cfg sip label Home; cfg sip voicemail "*97"; cfg sip enabled on
cfg sip2 user tom; cfg sip2 domain sip.club.example; cfg sip2 password another; cfg sip2 transport tls
cfg sip2 label Club; cfg sip2 enabled on
cfg sip2 forward_always on; cfg sip2 forward_always_to 2001
cfg phone default_account 1; cfg phone dnd on
cfg system timezone Europe/London
cfg audio ringer 9; cfg audio handset 10; cfg audio speaker 12; cfg audio headset 8
cfg screen brightness 8
cfg feed url "file://$SB/feeds/3/index.json"
printf '%s\n' 'Alice Example|1002' 'Bob Example|+441184960123' 'Club bar|2001' 'Front door|*8' 'Mum|01184960456' > "$T/phone/contacts"
now=$(date +%s)
printf 'seen %s\n%s|o|312|1002|Alice Example|1\n%s|i|64|2001|Club bar|2\n%s|m|0|07700900123||1\n%s|o|0|01184960456|Mum|1\n%s|i|1275|+441184960123|Bob Example|1\n' \
    $((now - 100)) $((now - 90000)) $((now - 80000)) $((now - 20000)) $((now - 7000)) $((now - 3000)) > "$T/phone/history"
echo "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOeStCLCiE0njMGEkRNUHaNmd+dNxoLC9NECKxV/wLQ9 tom@desk" > "$T/keep/ssh/authorized_keys"
printf 'dev=no\nssh=on\nhost_key=SHA256:6Ru8gBwrS00sNCkM/wL0/lOtLVJuGwKvLVdhZR08WAg (ed25519)\ndoor=closed\nkey=ssh-ed25519 tom@desk\nkeys=1\n' > "$T/access/status"
# two apps a release behind, for Update all
for a in demo memo; do "$SB/t48-app" --apps "$T/installed:$SB/apps" --config "$T/config" --data "$T/appdata" install $a >/dev/null; done
cfg feed url "file://$SB/feeds/4/index.json"
"$WEB" --listen "127.0.0.1:$PORT" --config "$T/config" --state "$T/phone" --apps "$T/installed:$SB/apps" --data "$T/appdata" \
    --apply /bin/true --app "$SB/t48-app" --access "$T/access" --keys "$T/keep" --run "$T/run" 2>"$T/log" &
PID=$!
sleep 0.5
node "$HERE/shots.js" "http://127.0.0.1:$PORT" "$T/run/status" "$T/shots"
# smaller files: the pages have few colours
python3 - "$T/shots" "$OUT" <<'PY'
import os, sys
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
for name in sorted(os.listdir(src)):
    im = Image.open(os.path.join(src, name)).convert("RGB").quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    im.save(os.path.join(out, name), optimize=True)
    print(name, os.path.getsize(os.path.join(out, name)))
PY
