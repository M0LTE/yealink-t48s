#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# The stand-in for t48-apply in the test with the simulator (with-phone.sh).
# Access commands go to the phone's real t48-access behind the shell tests'
# stand-ins for dropbear; and, as t48-apply and runit do on the phone, the web
# page is started when the setting says on. It leaves by itself when it says
# off. Run by the simulator from the shell's build folder.
HERE=$(cd "$(dirname "$0")" && pwd)
case "$1" in
access|access-*) sh "$HERE/../../shell/tests/access/apply.sh" "$@" ;;
*) exit 0 ;;
esac
[ "$1" = access ] || exit 0
[ "$(./t48-config --config simdata/config get access web 2>/dev/null)" = on ] || exit 0
[ -f simdata/web.pid ] && kill -0 "$(cat simdata/web.pid)" 2>/dev/null && exit 0
"$WEB_BIN" --listen "127.0.0.1:$WEB_PORT" --config simdata/config --state simdata/state \
    --apps simdata/installed:apps --data simdata/appdata --apply "$HERE/apply.sh" --app ./t48-app \
    --access simdata/access --keys simdata/access/keep --run simdata/web >> web.log 2>&1 &
echo $! > simdata/web.pid
