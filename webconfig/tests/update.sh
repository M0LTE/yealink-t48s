#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# The stand-in for t48-update in the tests of the Update page (test_web.py):
# "info" in the real one's form, made from files in the folder $FAKE, and
# check, install, kernel and channel that only change those files.
#   installed     "no": a phone started without installing
#   channel       the channel, as "channel" saved it
#   offer         what the next check finds: latest=, update=, kernel= lines
#   fail          the next check fails, with this cause
#   hold-STATE    while it exists a run waits in that state (checking,
#                 downloading, writing), so a test can look at the page
#   state, latest, result    what a run leaves, as the real one does
#   calls         every command this was run with, one a line
F=${FAKE:?}
echo "$*" >> "$F/calls"
get() { sed -n "s/^$2=//p" "$F/$1" 2>/dev/null | head -n 1; }
state() { printf 'state=%s\nmessage=%s\n' "$1" "$2" > "$F/state.new" && mv "$F/state.new" "$F/state"; }
result() { printf 'result=%s\nresult_time=%s\n' "$1" "$(date +%s)" > "$F/result"; }
# stay in a state while the test holds it there, a minute at most
stay() {
    state "$1" ""
    n=0
    while [ -e "$F/hold-$1" ] && [ $n -lt 600 ]; do sleep 0.1; n=$((n + 1)); done
}
check() {
    stay checking
    rm -f "$F/latest"
    if [ -e "$F/fail" ]; then
        result "$(cat "$F/fail")"; state failed "$(cat "$F/fail")"
        cat "$F/fail" >&2
        exit 1
    fi
    { cat "$F/offer"; echo "checked=$(date +%s)"; } > "$F/latest"
}

case "$1" in
info)
    if [ "$(cat "$F/installed" 2>/dev/null)" = no ]; then
        echo "installed=no"
    else
        printf 'installed=yes\nslot=a\nactive=a\ntrial=-\n'
    fi
    echo "version=v0.2.0"
    c=$(cat "$F/channel" 2>/dev/null)
    echo "channel=${c:-stable}"
    echo "url=https://example.org/${c:-stable}"
    if [ -e "$F/latest" ]; then
        cat "$F/latest"
    else
        printf 'latest=\nupdate=unknown\nkernel=unknown\nchecked=\n'
    fi
    st=$(get state state)
    echo "state=${st:-idle}"
    echo "message=$(get state message)"
    echo "result=$(get result result)"
    echo "result_time=$(get result result_time)" ;;
check)
    check
    state idle "" ;;
install)
    check
    stay downloading
    stay writing
    state restart "" ;;
kernel)
    check
    stay downloading
    stay writing
    rm -f "$F/latest"
    result "Kernel written"
    state restart "" ;;
channel)
    case "$2" in
    stable|testing) ;;
    http://?*|https://?*) case "$2" in *[!A-Za-z0-9:/._~%+-]*) echo "Not a URL" >&2; exit 1 ;; esac ;;
    *) echo "usage: t48-update channel [stable|testing|URL]" >&2; exit 1 ;;
    esac
    echo "$2" > "$F/channel"
    rm -f "$F/latest"
    echo "Channel: $2" ;;
*)  exit 2 ;;
esac
