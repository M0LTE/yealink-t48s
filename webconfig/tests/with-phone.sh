#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# with-phone.sh BUILD_DIR: the web page and the phone's shell together. The
# shell runs as its PC simulator, driven by phone.script; the web page is the
# real program, started when the script turns it on at the phone's screen; and
# browser.py plays the owner's browser. Pictures land in BUILD_DIR/shots.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
O=$(basename "$1")
WEB_BIN=$(cd "$HERE/../$O" && pwd)/t48-web
SHOTS=$(cd "$HERE/../$O" && mkdir -p shots && cd shots && pwd)
export WEB_BIN WEB_PORT=${WEB_PORT:-8437} T48_TESTS="$HERE"
export B="python3 $HERE/browser.py simdata/jar http://127.0.0.1:$WEB_PORT"
cd "$HERE/../../shell/$O"
rm -rf simdata web.log
mkdir -p shots
status=0
T48_BACKLIGHT=simdata/bl ./t48-sim --apps simdata/installed:apps --data simdata/appdata --config simdata/config \
    --state simdata/state --access simdata/access --web simdata/web --apply "$HERE/apply.sh" \
    --script "$HERE/phone.script" 2> sim.log || status=1
[ -f simdata/web.pid ] && kill "$(cat simdata/web.pid)" 2>/dev/null || true
cat sim.log
echo "---- the web page's log ----"
cat web.log 2>/dev/null || true
cp shots/web-*.png "$SHOTS/" 2>/dev/null || true
# nothing that was typed as a password may be in either log
if grep -q "hunter2\|pin-4711" sim.log web.log; then echo "a password is in a log"; exit 1; fi
[ $status = 0 ] || { echo "the test with the simulator FAILED"; exit 1; }
echo "the web page and the simulator agree"
