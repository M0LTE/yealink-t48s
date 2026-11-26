#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Registering again when a network link comes back (issue #77, t48keepalive,
# docs/sip.md#registration-back-off). A dummy link d0 stands in for the
# phone's Ethernet: its carrier is taken down and brought up, as when a cable
# is pulled and put back. Nothing here waits for a time to pass: each step
# waits for the line it expects, with a cap so a failure cannot hang.
#
#   1. alice registers with the test Kamailio; bob's server refuses him
#      (TCP, nothing listening), so he is in libre's back-off of 30 s or
#      more. The link's MTU changes (not the carrier: nothing happens), then
#      the carrier goes down and up: alice refreshes her registration, bob
#      tries again at once; and t48-linkd, watching d0, runs the link-up
#      command once (t48-apply link-up on the phone; a stand-in here) without
#      passing on any descriptor of its own
#   2. down and up again at once: put off to 30 s after the first (the line
#      saying so is the event waited for; nothing else has happened)
#
#   docker run --cap-add NET_ADMIN ... t48-sip sip/test/link-test.sh
#                               after TARGET=host sip/build.sh. Seconds.
set -e
SIP=$(cd "$(dirname "$0")/.." && pwd)
T=${T:-$HOME/src-build/t48s}
HOST_BARESIP=${HOST_BARESIP:-$T/sip-host/bin/baresip}
W=/tmp/t48-sip-test    # kamailio.cfg puts its control socket here
rm -rf $W && mkdir -p $W
FAILS=0

ip link add d0 type dummy 2>/dev/null || ip link show d0 >/dev/null 2>&1 ||
    { echo "link-test: needs ip and NET_ADMIN (docker run --cap-add NET_ADMIN)" >&2; exit 1; }
ip link set d0 up

PIDS=
cleanup() { kill $PIDS $KAM 2>/dev/null || true; wait 2>/dev/null || true; ip link del d0 2>/dev/null || true; }
trap cleanup EXIT

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }

# wait_count FILE PATTERN N: until FILE has N lines matching (the event, not a
# time), giving up after 15 s so a failure cannot hang
wait_count() {
    i=0
    until [ "$(grep -c "$2" "$1" 2>/dev/null || true)" -ge "$3" ] || [ $i -ge 150 ]; do
        sleep 0.1; i=$((i + 1))
    done
    [ "$(grep -c "$2" "$1" 2>/dev/null || true)" -ge "$3" ]
}

cycle() { ip link set d0 carrier off; ip link set d0 carrier on; }

kamailio -f "$SIP/test/kamailio.cfg" -DD -E -Y $W -w $W >$W/kamailio.log 2>&1 &
KAM=$!

# instance NAME PORT TRANSPORT DOMAIN
instance() {
    d=$W/$1
    mkdir -p $d
    sed -e "s|^sip_listen.*|sip_listen\t\t127.0.0.1:$2|" \
        -e "s|^rtp_ports.*|rtp_ports\t\t$(( ($2 - 5070) * 1000 + 20000 ))-$(( ($2 - 5070) * 1000 + 20099 ))|" \
        -e "s|^audio_source.*|audio_source\t\tausine,440|" \
        -e "s|^audio_player.*|audio_player\t\taufile,/dev/null|" \
        "$SIP/config/config" > $d/config.in
    printf 'net_interface\t\t127.0.0.1\n' >> $d/config.in
    printf 'user=%s\npassword=test-only-%s\ndomain=%s\ntransport=%s\nmwi=off\n' \
        "$1" "$1" "$4" "$3" > $d/sip
    T48_SIP_SETTINGS=$d/sip T48_SIP_CONFIG=$d/config.in T48_SIP_RUN=$d/run BARESIP=$HOST_BARESIP \
        "$SIP/t48-sip" >>$d/log 2>&1 &
    PIDS="$PIDS $!"
}

# t48-linkd, built here, with a stand-in for t48-apply that logs its argument
# and any socket it was given (the netlink one would show)
cc -O1 -I"$SIP/../native" -o $W/t48-linkd "$SIP/../native/t48-linkd.c"
printf '#!/bin/sh\necho "$*" >> %s\nls -l /proc/$$/fd | grep socket: | sed "s/^/fd /" >> %s\n' \
    "$W/linkup.log" "$W/linkup.log" > $W/apply
chmod +x $W/apply
: > $W/linkup.log
$W/t48-linkd -i d0 -c $W/apply > $W/linkd.log 2>&1 &
PIDS="$PIDS $!"
wait_count $W/linkd.log "watching d0" 1 || true

# Kamailio's socket first
i=0
until ss -lun | grep -q '127.0.0.1:5060 ' || [ $i -ge 150 ]; do sleep 0.1; i=$((i + 1)); done
instance alice 5071 udp 127.0.0.1
instance bob 5072 tcp 127.0.0.1:5199

echo "== 1. registered, and backed off; carrier down and up"
if wait_count $W/kamailio.log "REGISTER alice .*saved" 1; then pass "alice registered"; else fail "alice registered"; fi
wait_count $W/bob/log "Register: Connection refused" 1 || true
ip link set d0 mtu 1400
cycle
if wait_count $W/kamailio.log "REGISTER alice .*saved" 2; then pass "alice registers again when the carrier is back"; else fail "alice registers again when the carrier is back"; fi
if wait_count $W/bob/log "Register: Connection refused" 2; then pass "bob tries again at once, not after his back-off"; else fail "bob tries again at once, not after his back-off"; fi
if wait_count $W/linkup.log "^link-up$" 1 && [ "$(grep -c "^fd" $W/linkup.log)" -eq 0 ]; then pass "t48-linkd runs link-up, passing no descriptors"; else fail "t48-linkd runs link-up, passing no descriptors"; sed 's/^/  /' $W/linkup.log; fi
n=$(grep -c "t48keepalive: d0 up, registering again" $W/alice/log || true)
if [ "$n" -eq 1 ]; then pass "once: the MTU change did nothing"; else fail "once: the MTU change did nothing ($n)"; fi
grep -h "t48keepalive: d0" $W/alice/log | sed 's/^/  log: /'

echo "== 2. down and up again at once"
cycle
if wait_count $W/alice/log "d0 up again soon after, waiting" 1 &&
   [ "$(grep -c "d0 up, registering again" $W/alice/log)" -eq 1 ] &&
   wait_count $W/linkd.log "d0 up again soon after, waiting" 1 &&
   [ "$(grep -c "^link-up$" $W/linkup.log)" -eq 1 ]; then
    pass "put off, not acted on"
else
    fail "put off, not acted on"
fi
grep -h "soon after" $W/alice/log | sed 's/^/  log: /'
sed 's/^/  linkd: /' $W/linkd.log

[ $FAILS -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit $FAILS
