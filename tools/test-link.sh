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
#   for its address and does not hold the lock; a fixed address ignores
#   link-up; a renewal leaves the route and ntpd alone unless they change
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
O=$HERE/buildroot/board/t48s/rootfs-overlay
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }

mkdir -p "$T/run" "$T/etc" "$T/dev" "$T/bin"
: > "$T/log"
stub() { printf '#!/bin/sh\n%s\n' "$2" > "$T/bin/$1"; chmod +x "$T/bin/$1"; }
L=$T/log
stub killall 'echo "killall $*" >> '"$L"
stub ntpd 'echo "ntpd $*" >> '"$L"
stub pidof '[ -e '"$T/ntpd-running"' ]'
stub t48-config 'case "$2 $3" in "network mode") cat '"$T/mode"' 2>/dev/null || exit 1 ;; "network address") cat '"$T/fixed"' 2>/dev/null || exit 1 ;; *) exit 1 ;; esac'
stub udhcpc 'echo "udhcpc $*" >> '"$L"'; [ -e /proc/self/fd/9 ] && echo "udhcpc holds fd 9" >> '"$L"'; true'
# ifconfig IF: says the address; ifconfig IF ADDR ...: sets it
stub ifconfig 'if [ $# -eq 1 ]; then a=$(cat '"$T/addr"' 2>/dev/null); [ -z "$a" ] || echo "inet addr:$a  Mask:255.255.255.0"; else [ "$2" = 0.0.0.0 ] && : > '"$T/addr"' || echo "$2" > '"$T/addr"'; echo "ifconfig $*" >> '"$L"'; fi'
# route -n: the default route from a file; add and del logged
stub route 'case "$1" in -n) g=$(cat '"$T/gw"' 2>/dev/null); [ -z "$g" ] || echo "0.0.0.0 $g 0.0.0.0 UG 0 0 0 eth0" ;; del) [ -s '"$T/gw"' ] || exit 1; : > '"$T/gw"'; echo "route del" >> '"$L"' ;; add) echo "$4" > '"$T/gw"'; echo "route add $4" >> '"$L"' ;; esac'
# t48-apply sip is what restarts baresip: logged; the rest is the real one
cp "$O/usr/bin/t48-apply" "$T/t48-apply-real"
stub t48-apply '[ "$1" = sip ] && { echo "t48-apply sip" >> '"$L"'; exit 0; }; exec sh '"$T/t48-apply-real"' "$@"'
export PATH="$T/bin:$PATH" T48_ROOT="$T" interface=eth0

dhcp() { sh "$O/usr/share/udhcpc/default.script" "$@"; wait; }
apply() { sh "$T/t48-apply-real" "$@"; wait; }
count() { grep -c "^$1" "$L" || true; }

# the cable back, same network
echo 10.0.0.5 > "$T/addr"
apply link-up
[ "$(cat "$T/run/t48-dhcp-kick")" = 10.0.0.5 ] || fail "link-up names the address in the kick file"
grep -q "^udhcpc .*-r 10.0.0.5" "$L" && ! grep -q "^udhcpc .* -q" "$L" || fail "udhcpc stays running and asks for its address"
[ "$(count "udhcpc holds")" = 0 ] || fail "udhcpc does not hold the lock"
pass "link-up: udhcpc started again without -q, asking for 10.0.0.5, not holding the lock"
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
pass "a renewal changes the route and ntpd only when they change"

# the cable back on another network
apply link-up
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
[ ! -e "$T/run/t48-dhcp-kick" ] || fail "network clears a stale kick"
pass "network clears a stale kick"

# a fixed address: link-up has nothing to ask for
echo static > "$T/mode"; echo 192.168.7.50 > "$T/fixed"
n=$(count udhcpc); k=$(count ifconfig)
apply link-up
[ "$(count udhcpc)" = "$n" ] && [ "$(count ifconfig)" = "$k" ] && [ ! -e "$T/run/t48-dhcp-kick" ] || fail "fixed address: link-up does nothing"
pass "with a fixed address link-up does nothing"
