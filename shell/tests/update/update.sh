#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# The simulator's stand-in for t48-update (make check gives it as --update):
# info, check, install, kernel and channel as the Update page uses them
# (docs/settings.md), with no network and no flash. Everything is a file under
# simdata/update, which a test script writes to set the scene:
#   installed     yes (also when missing) or no
#   slot version  what is running
#   offer         the version the channel has; missing, the check fails
#   kernel_offer  same (also when missing) or differs
#   result        the last result, as two lines: result=..., result_time=...
# and which this script writes: channel, state, asked (one line for each
# command run), and simdata/state/update-latest, where the real one keeps what
# the last check found and the shell's home screen looks for a change.
D=$PWD/simdata/update
L=$PWD/simdata/state/update-latest
mkdir -p "$D" "${L%/*}"
get() { cat "$D/$1" 2>/dev/null || echo "$2"; }
state() { printf 'state=%s\nmessage=%s\n' "$1" "$2" > "$D/state.new" && mv "$D/state.new" "$D/state"; }
result() { printf 'result=%s\nresult_time=%s\n' "$1" "$(date +%s)" > "$D/result"; }
stop() { state failed "$1"; result "$1"; echo "$1" >&2; exit 1; }
check() {
    offer=$(get offer "")
    [ -n "$offer" ] || return 1
    if [ "$offer" = "$(get version 20261124-ac7a8f4)" ]; then u=no; else u=yes; fi
    printf 'latest=%s\nupdate=%s\nkernel=%s\nchecked=%s\n' "$offer" "$u" "$(get kernel_offer same)" "$(date +%s)" > "$L"
}

[ "$1" = info ] || echo "$*" >> "$D/asked"
case "$1" in
info)
    if [ "$(get installed yes)" = yes ]; then
        printf 'installed=yes\nslot=%s\nactive=%s\ntrial=-\n' "$(get slot a)" "$(get slot a)"
    else
        echo "installed=no"
    fi
    echo "version=$(get version 20261124-ac7a8f4)"
    c=$(get channel stable)
    echo "channel=$c"
    case "$c" in stable|testing) echo "url=https://example.org/$c" ;; *) echo "url=$c" ;; esac
    cat "$L" 2>/dev/null || printf 'latest=\nupdate=unknown\nkernel=unknown\nchecked=\n'
    cat "$D/state" 2>/dev/null || printf 'state=idle\nmessage=\n'
    cat "$D/result" 2>/dev/null || printf 'result=\nresult_time=\n' ;;
check)
    state checking ""
    sleep 1
    check || stop "No release found"
    state idle "" ;;
install)
    state downloading ""
    sleep 1
    check || stop "No release found"
    state writing ""
    sleep 2
    state restart "" ;;
kernel)
    state downloading ""
    sleep 1
    state writing ""
    sleep 2
    result "Kernel written"
    state restart "" ;;
channel)
    echo "$2" > "$D/channel"
    rm -f "$L" ;;
*) exit 2 ;;
esac
