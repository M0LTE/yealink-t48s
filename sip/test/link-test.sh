#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Registering again when a network link comes back (issue #77, t48keepalive,
# docs/sip.md#registration-back-off). A dummy link d0 stands in for the
# phone's Ethernet: its carrier is taken down and brought up, as when a cable
# is pulled and put back. Nothing here waits for a time to pass: each step
# waits for the line it expects, with a cap so a failure cannot hang.
#
#   1. alice registers with the test Kamailio; the carrier goes down and up:
#      she registers again at once
#   2. bob's server refuses him (TCP, nothing listening), so he is in
#      libre's back-off of 30 s or more; carrier down and up: he tries again
#      at once
#   3. a change that is not the carrier (the link's MTU) registers no one:
#      after it and one more carrier cycle there are exactly three
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

# wait_count FILE PATTERN N: until FILE has N lines matching, at most 15 s
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

sleep 1    # Kamailio's sockets
instance alice 5071 udp 127.0.0.1
instance bob 5072 tcp 127.0.0.1:5199

echo "== 1. registered, carrier down and up"
if wait_count $W/kamailio.log "REGISTER alice .*saved" 1; then pass "alice registered"; else fail "alice registered"; fi
wait_count $W/bob/log "Register: Connection refused" 1 || true
cycle
if wait_count $W/kamailio.log "REGISTER alice .*saved" 2; then pass "alice registers again when the carrier is back"; else fail "alice registers again when the carrier is back"; fi
grep -h "t48keepalive: d0" $W/alice/log | sed 's/^/  log: /'

echo "== 2. backed off, carrier down and up"
# bob failed once before the cycle above and once at it; libre waits 30 s or
# more after a failure, so a third failure now is the next cycle's doing
wait_count $W/bob/log "Register: Connection refused" 2 || true
cycle
if wait_count $W/bob/log "Register: Connection refused" 3; then pass "bob tries again at once, not after his back-off"; else fail "bob tries again at once, not after his back-off"; fi

echo "== 3. not the carrier"
ip link set d0 mtu 1400
cycle
wait_count $W/kamailio.log "REGISTER alice .*saved" 3 || true
n=$(grep -c "t48keepalive: d0 up" $W/alice/log || true)
if [ "$n" -eq 3 ]; then pass "only carrier changes register (3)"; else fail "only carrier changes register ($n, not 3)"; fi

[ $FAILS -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit $FAILS
