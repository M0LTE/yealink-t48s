#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# shots.sh BUILD_DIR: pictures of the web page for docs/web.md, into docs/img.
# A made-up phone (accounts, contacts, calls, two apps with updates, a firmware update, a key, do not disturb) is served by the
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
# the official feed is one on disk with no apps (tests/mkfeeds.sh); two more feeds are added, a club's
# (a folder pointed at feed 3, then 4, so it has updates) and another that shares an app with it
export T48_OFFICIAL_FEED="file://$SB/feeds/official/index.json"
ln -s "$SB/feeds/3" "$T/radio-club"
ln -s "$SB/feeds/7" "$T/extras"
# and in the picture of the Apps page they get made-up web addresses (shots.js)
export SHOT_REPLACE=$(python3 -c 'import json, sys; t, sb = sys.argv[1:]; print(json.dumps([
    ["file://%s/feeds/official/index.json" % sb, "https://m0lte.github.io/yealink-t48s-apps/index.json"],
    ["file://%s/radio-club/index.json" % t, "https://apps.radioclub.example/index.json"], ["radio-club", "apps.radioclub.example"],
    ["file://%s/extras/index.json" % t, "https://t48.example.net/index.json"], ["extras", "t48.example.net"]]))' "$T" "$SB")
mkdir -p "$T/config" "$T/phone" "$T/access" "$T/keep/ssh" "$T/installed"
cfg access web on
cfg access ssh on
cfg sip user 1001; cfg sip domain pbx.example.org; cfg sip password not-a-real-one
cfg sip display_name "Tom M0LTE"; cfg sip label Home; cfg sip voicemail "*97"; cfg sip enabled on
cfg sip2 user tom; cfg sip2 domain sip.club.example; cfg sip2 password another; cfg sip2 transport tls
cfg sip2 label Club; cfg sip2 enabled on
cfg sip2 forward_always on; cfg sip2 forward_always_to 2001
cfg sip2 srtp optional
# two roots added for SIP over TLS (issue #78): test-only certificates, one expired
mkdir -p "$T/config/certs"
cp "$HERE/../../shell/tests/certs/test-root.pem" "$T/config/certs/1111111111111111.pem"
cp "$HERE/../../shell/tests/certs/expired-root.pem" "$T/config/certs/2222222222222222.pem"
cfg phone default_account 1; cfg phone dnd on
cfg system timezone Europe/London
cfg audio ringer 9; cfg audio handset 10; cfg audio speaker 12; cfg audio headset 8
cfg screen brightness 8
cfg feed url1 "file://$T/radio-club/index.json"; cfg feed url2 "file://$T/extras/index.json"
printf '%s\n' 'Alice Example|1002' 'Bob Example|+441184960123' 'Club bar|2001' 'Front door|*8' 'Mum|01184960456' > "$T/phone/contacts"
now=$(date +%s)
printf 'seen %s\n%s|o|312|1002|Alice Example|1\n%s|i|64|2001|Club bar|2\n%s|m|0|07700900123||1\n%s|o|0|01184960456|Mum|1\n%s|i|1275|+441184960123|Bob Example|1\n' \
    $((now - 100)) $((now - 90000)) $((now - 80000)) $((now - 20000)) $((now - 7000)) $((now - 3000)) > "$T/phone/history"
echo "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOeStCLCiE0njMGEkRNUHaNmd+dNxoLC9NECKxV/wLQ9 tom@desk" > "$T/keep/ssh/authorized_keys"
printf 'dev=no\nssh=on\nhost_key=SHA256:6Ru8gBwrS00sNCkM/wL0/lOtLVJuGwKvLVdhZR08WAg (ed25519)\ndoor=closed\nkey=ssh-ed25519 tom@desk\nkeys=1\n' > "$T/access/status"
# two apps a release behind, for Update all
for a in demo memo; do "$SB/t48-app" --apps "$T/installed:$SB/apps" --config "$T/config" --data "$T/appdata" --state "$T/phone" install $a >/dev/null; done
ln -sfn "$SB/feeds/4" "$T/radio-club"
"$SB/t48-app" --apps "$T/installed:$SB/apps" --config "$T/config" --data "$T/appdata" --state "$T/phone" check >/dev/null
# the stand-in for t48-update (tests/update.sh), with a release the daily check has found
mkdir -p "$T/update"
printf 'latest=20261126-1a2b3c4\nupdate=yes\nkernel=same\nchecked=%s\n' "$now" > "$T/update/latest"
printf '#!/bin/sh\nFAKE="%s" exec sh "%s" "$@"\n' "$T/update" "$HERE/update.sh" > "$T/update.sh"
chmod +x "$T/update.sh"
"$WEB" --listen "127.0.0.1:$PORT" --config "$T/config" --state "$T/phone" --apps "$T/installed:$SB/apps" --data "$T/appdata" \
    --apply /bin/true --app "$SB/t48-app" --access "$T/access" --keys "$T/keep" --run "$T/run" --update "$T/update.sh" 2>"$T/log" &
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
