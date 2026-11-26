#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-link.sh: the network side of a cable put back (issue #77, docs/sip.md):
# t48-apply network and link-up, and the udhcpc script, on any PC, no root.
# ifconfig, route, ntpd, pidof, killall, udhcpc and t48-apply sip are
# stand-ins that keep a log. No waits: each step is one run of a script.
#   link-up keeps the address while DHCP asks and names it in the kick file;
#   the same address back leaves baresip alone, a new one restarts it; a
#   lease that really ends, or a kick naming another address, takes the
#   address away; network clears a stale kick; udhcpc runs without -q, asks
#   for its address, and like ntpd keeps no descriptor of its caller's; a
#   fixed address ignores link-up; a renewal leaves the route and ntpd alone
#   unless they change
# SH: the shell the scripts run under; the phone's is busybox ash, so
#   SH="busybox ash" sh tools/test-link.sh   is the closer test (CI runs both)
set -e
SH=${SH:-sh}
HERE=$(cd "$(dirname "$0")/.." && pwd)
O=$HERE/buildroot/board/t48s/rootfs-overlay
T=$(mktemp -d)
trap '[ -n "$KEEP" ] || rm -rf "$T"' EXIT
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }

mkdir -p "$T/run" "$T/etc" "$T/dev" "$T/bin"
: > "$T/log"
stub() { printf '#!/bin/sh\n%s\n' "$2" > "$T/bin/$1"; chmod +x "$T/bin/$1"; }
L=$T/log
stub killall 'echo "killall $*" >> '"$L"
stub ntpd 'echo "ntpd $*" >> '"$L"'; for n in 7 9; do [ -e /proc/$$/fd/$n ] && echo "leak: ntpd has fd $n" >> '"$L"'; done; true'
stub pidof '[ -e '"$T/ntpd-running"' ]'
stub t48-config 'case "$2 $3" in "network mode") cat '"$T/mode"' 2>/dev/null || exit 1 ;; "network address") cat '"$T/fixed"' 2>/dev/null || exit 1 ;; *) exit 1 ;; esac'
stub udhcpc 'echo "udhcpc $*" >> '"$L"'; for n in 7 9; do [ -e /proc/$$/fd/$n ] && echo "leak: udhcpc has fd $n" >> '"$L"'; done; true'
# ifconfig IF: says the address; ifconfig IF ADDR ...: sets it
stub ifconfig 'if [ $# -eq 1 ]; then a=$(cat '"$T/addr"' 2>/dev/null); [ -z "$a" ] || echo "inet addr:$a  Mask:255.255.255.0"; else [ "$2" = 0.0.0.0 ] && : > '"$T/addr"' || echo "$2" > '"$T/addr"'; echo "ifconfig $*" >> '"$L"'; fi'
# route -n: the default route from a file; add and del logged
stub route 'case "$1" in -n) g=$(cat '"$T/gw"' 2>/dev/null); [ -z "$g" ] || echo "0.0.0.0 $g 0.0.0.0 UG 0 0 0 eth0" ;; del) [ -s '"$T/gw"' ] || exit 1; : > '"$T/gw"'; echo "route del" >> '"$L"' ;; add) echo "$4" > '"$T/gw"'; echo "route add $4" >> '"$L"' ;; esac'
# t48-apply sip is what restarts baresip: logged; the rest is the real one
cp "$O/usr/bin/t48-apply" "$T/t48-apply-real"
stub t48-apply '[ "$1" = sip ] && { echo "t48-apply sip" >> '"$L"'; exit 0; }; exec $SH '"$T/t48-apply-real"' "$@"'
export PATH="$T/bin:$PATH" T48_ROOT="$T" interface=eth0
# a descriptor of the caller's that the daemons must not keep (as udhcpc's
# own sockets are, when it runs the DHCP script)
exec 7>"$T/caller-fd"
# the stand-ins must be what the scripts run: a busybox built with its
# standalone shell runs its own ifconfig and udhcpc first (Ubuntu's does),
# which as root would change this machine's network. The image's busybox has
# it off (Buildroot's busybox.config, and nothing in board/t48s/busybox.fragment).
for c in ifconfig udhcpc route; do
    [ "$($SH -c "command -v $c")" = "$T/bin/$c" ] ||
        { echo "test-link: $SH runs its own $c, not the stand-in; use a shell without busybox's standalone mode" >&2; exit 2; }
done

# under a time limit: a script that hangs is a failure, not a hung test
dhcp() { timeout 10 $SH "$O/usr/share/udhcpc/default.script" "$@" || fail "the DHCP script failed or hung"; }
apply() { timeout 10 $SH "$T/t48-apply-real" "$@" || fail "t48-apply failed or hung"; }
count() { grep -c "^$1" "$L" || true; }
# udhcpc is started in the background and t48-apply does not wait for it:
# wait for its line (the event), giving up after 5 s
started() { i=0; until [ "$(count udhcpc)" -ge "$1" ] || [ $i -ge 100 ]; do sleep 0.05; i=$((i + 1)); done; }

# the cable back, same network
echo 10.0.0.5 > "$T/addr"
apply link-up
started 1
[ "$(cat "$T/run/t48-dhcp-kick")" = 10.0.0.5 ] || fail "link-up names the address in the kick file"
grep -q "^udhcpc .*-r 10.0.0.5" "$L" && ! grep -q "^udhcpc .* -q" "$L" || fail "udhcpc stays running and asks for its address"
[ "$(count "leak")" = 0 ] || fail "udhcpc given no descriptors"
pass "link-up: udhcpc started again without -q, asking for 10.0.0.5, holding neither the lock nor anything else"
dhcp deconfig
[ "$(cat "$T/addr")" = 10.0.0.5 ] && [ ! -e "$T/run/t48-dhcp-kick" ] || fail "address kept while DHCP asks"
pass "address kept while DHCP asks"
ip=10.0.0.5 router=10.0.0.1 dhcp bound
[ "$(count "t48-apply sip")" = 0 ] || fail "same address: baresip left alone"
pass "same address back: baresip left alone"

# a renewal with the same router and time servers
n=$(count route); : > "$T/ntpd-running"; m=$(count ntpd)
ip=10.0.0.5 router=10.0.0.1 dhcp renew
[ "$(count route)" = "$n" ] && [ "$(count ntpd)" = "$m" ] || fail "renewal: route and ntpd left alone"
ip=10.0.0.5 router=10.0.0.254 ntpsrv=10.0.0.1 dhcp renew
[ "$(cat "$T/gw")" = 10.0.0.254 ] && [ "$(count ntpd)" = $((m + 1)) ] || fail "renewal: new router and time server taken"
[ "$(count "leak")" = 0 ] || fail "ntpd given no descriptors"
pass "a renewal changes the route and ntpd only when they change; ntpd keeps none of the script's descriptors"

# the cable back on another network
apply link-up
started 2
dhcp deconfig
ip=192.168.7.20 router=192.168.7.1 dhcp bound
[ "$(count "t48-apply sip")" = 1 ] && [ "$(cat "$T/addr")" = 192.168.7.20 ] || fail "new network: new address, baresip restarted"
pass "another network: its address, baresip started again"

# a lease that ends, and a kick that names another address
dhcp deconfig
[ -z "$(cat "$T/addr")" ] || fail "lease ended: address removed"
echo 192.168.7.20 > "$T/addr"; echo 10.9.9.9 > "$T/run/t48-dhcp-kick"
dhcp deconfig
[ -z "$(cat "$T/addr")" ] || fail "kick for another address: address removed"
pass "a lease that ends, or a kick for another address, takes the address away"

# network (the Network page, boot) clears a stale kick
echo 192.168.7.20 > "$T/addr"; echo 192.168.7.20 > "$T/run/t48-dhcp-kick"
apply network
started 3
[ ! -e "$T/run/t48-dhcp-kick" ] || fail "network clears a stale kick"
pass "network clears a stale kick"

# a fixed address: link-up has nothing to ask for
echo static > "$T/mode"; echo 192.168.7.50 > "$T/fixed"
n=$(count udhcpc); k=$(count ifconfig)
apply link-up
[ "$(count udhcpc)" = "$n" ] && [ "$(count ifconfig)" = "$k" ] && [ ! -e "$T/run/t48-dhcp-kick" ] || fail "fixed address: link-up does nothing"
pass "with a fixed address link-up does nothing"
