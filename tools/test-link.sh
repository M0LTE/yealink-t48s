#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# test-link.sh: t48-link and the DHCP script (issue #77, docs/sip.md) on any PC,
# no root. The carrier is a file standing in for sysfs; ifconfig, route, ntpd,
# pidof, killall and t48-apply are stand-ins that keep a log. No waits: each
# step is one look (t48-link once) or one run of the DHCP script.
#   cable back: a new lease is asked for, once, and the address is kept while
#   it is; the first look and a carrier that stays up do nothing; the same
#   address back from DHCP leaves baresip alone, a new one restarts it; a
#   lease that really ends takes the address away; a renewal with the same
#   time servers leaves ntpd alone
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
O=$HERE/buildroot/board/t48s/rootfs-overlay
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }

mkdir -p "$T/sys/class/net/eth0" "$T/run" "$T/etc" "$T/dev" "$T/bin"
: > "$T/log"
stub() { printf '#!/bin/sh\n%s\n' "$2" > "$T/bin/$1"; chmod +x "$T/bin/$1"; }
stub t48-apply 'echo "t48-apply $*" >> '"$T/log"
stub route ':'
stub killall 'echo "killall $*" >> '"$T/log"
stub ntpd 'echo "ntpd $*" >> '"$T/log"
stub pidof '[ -e '"$T/ntpd-running"' ]'
# ifconfig IF: says the address; ifconfig IF ADDR ...: sets it
stub ifconfig 'if [ $# -eq 1 ]; then a=$(cat '"$T/addr"' 2>/dev/null); [ -z "$a" ] || echo "inet addr:$a  Mask:255.255.255.0"; else [ "$2" = 0.0.0.0 ] && : > '"$T/addr"' || echo "$2" > '"$T/addr"'; echo "ifconfig $*" >> '"$T/log"'; fi'
export PATH="$T/bin:$PATH" T48_ROOT="$T" interface=eth0

link() { echo "$1" > "$T/sys/class/net/eth0/carrier"; sh "$O/usr/bin/t48-link" once >/dev/null; }
dhcp() { sh "$O/usr/share/udhcpc/default.script" "$@"; wait; }
applies() { grep -c "^t48-apply $1" "$T/log" || true; }

echo 10.0.0.5 > "$T/addr"
link 1
[ "$(applies network)" = 0 ] || fail "first look does nothing"
link 1
[ "$(applies network)" = 0 ] || fail "carrier staying up does nothing"
link 0
[ "$(applies network)" = 0 ] || fail "carrier down does nothing"
link 1
[ "$(applies network)" = 1 ] && [ -e "$T/run/t48-dhcp-kick" ] || fail "cable back: network applied again"
pass "cable back: network applied again, once; not on the first look or while up"

# the new udhcpc starts with deconfig: the address stays
dhcp deconfig
[ "$(cat "$T/addr")" = 10.0.0.5 ] && [ ! -e "$T/run/t48-dhcp-kick" ] || fail "address kept while DHCP asks"
pass "address kept while DHCP asks"

ip=10.0.0.5 dhcp bound
[ "$(applies sip)" = 0 ] || fail "same address: baresip left alone"
pass "same address back: baresip left alone"

link 0; link 1; dhcp deconfig
ip=192.168.7.20 dhcp bound
[ "$(applies sip)" = 1 ] && [ "$(cat "$T/addr")" = 192.168.7.20 ] || fail "new network: new address, baresip restarted"
pass "another network: its address, baresip started again"

dhcp deconfig
[ -z "$(cat "$T/addr")" ] || fail "lease ended: address removed"
pass "a lease that ends without a kick takes the address away"

: > "$T/ntpd-running"
n=$(grep -c "^ntpd" "$T/log")
ip=192.168.7.20 dhcp renew
[ "$(grep -c "^ntpd" "$T/log")" = "$n" ] || fail "renewal: ntpd left alone"
ntpsrv=10.0.0.1 ip=192.168.7.20 dhcp renew
[ "$(grep -c "^ntpd" "$T/log")" = $((n + 1)) ] || fail "new time server: ntpd started again"
pass "renewal leaves ntpd alone unless the time servers change"

# t48-apply network with DHCP: udhcpc stays running (no -q) and asks for the
# address it has
stub t48-config 'exit 1'
stub udhcpc 'echo "udhcpc $*" >> '"$T/log"
echo 192.168.7.20 > "$T/addr"
sh "$O/usr/bin/t48-apply" network
wait
grep -q "^udhcpc .*-r 192.168.7.20" "$T/log" && ! grep -q "^udhcpc .* -q" "$T/log" || fail "udhcpc stays running, asks for its address"
pass "udhcpc stays running to renew, and asks for the address it has"
