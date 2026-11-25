#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# NAT keepalives (issue #77, sip/modules/t48keepalive, docs/sip.md), on
# loopback with the throwaway Kamailio of call-test.sh. alice is behind a
# router's UDP NAT (nat.py) that drops a binding after 40 s with nothing
# going out; bob is on TCP with no NAT. Both register for 600 s, so only the
# keepalives cross the NAT between registrations.
#
#   1. keepalives on: after 130 s alice has sent OPTIONS every 23 to 29 s,
#      bob one after 96 to 120 s, the binding is still there, and bob's call
#      reaches alice
#   2. the router restarts (all bindings gone): alice's next OPTIONS comes
#      back with a new public port, she registers again, and bob's call
#      reaches her
#   3. keepalive=off: the binding goes after 40 s and bob's call does not
#      reach alice (the NAT stand-in is real)
#
#   sip/test/nat-test.sh        in the sip/Dockerfile container, after
#                               TARGET=host sip/build.sh. About 4 minutes.
set -e
SIP=$(cd "$(dirname "$0")/.." && pwd)
T=${T:-$HOME/src-build/t48s}
HOST_BARESIP=${HOST_BARESIP:-$T/sip-host/bin/baresip}
W=/tmp/t48-sip-test
rm -rf $W/* && mkdir -p $W
FAILS=0

PIDS=
cleanup() { kill $PIDS $KAM $NAT 2>/dev/null || true; }
trap cleanup EXIT

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }

kamailio -f "$SIP/test/kamailio.cfg" -DD -E -Y $W -w $W >$W/kamailio.log 2>&1 &
KAM=$!
python3 -u "$SIP/test/nat.py" 5090 5060 40 >$W/nat.log 2>&1 &
NAT=$!
sleep 1

# instance NAME PORT TRANSPORT DOMAIN [EXTRA SETTING]
instance() {
    d=$W/$1
    mkdir -p $d
    sed -e "s|^sip_listen.*|sip_listen\t\t127.0.0.1:$2|" \
        -e "s|^rtp_ports.*|rtp_ports\t\t$(( ($2 - 5070) * 1000 + 20000 ))-$(( ($2 - 5070) * 1000 + 20099 ))|" \
        -e "s|^audio_source.*|audio_source\t\tausine,440|" \
        -e "s|^audio_player.*|audio_player\t\taufile,/dev/null|" \
        "$SIP/config/config" > $d/config.in
    printf 'net_interface\t\t127.0.0.1\n' >> $d/config.in
    printf 'user=%s\npassword=test-only-%s\ndomain=%s\ntransport=%s\nregister_seconds=600\nmwi=off\n%s\n' \
        "$1" "$1" "$4" "$3" "$5" > $d/sip
    T48_SIP_SETTINGS=$d/sip T48_SIP_CONFIG=$d/config.in T48_SIP_RUN=$d/run BARESIP=$HOST_BARESIP \
        "$SIP/t48-sip" >>$d/log 2>&1 &
    eval "PID_$1=$!"
    PIDS="$PIDS $!"
}

# bob calls alice: exit 0 if alice rings
bob_calls_alice() {
    PYTHONPATH="$SIP/test" python3 -c '
import sys, time
from ctrl import Ctrl
w = sys.argv[1]
alice, bob = Ctrl("alice", w + "/alice/run/ctrl.sock"), Ctrl("bob", w + "/bob/run/ctrl.sock")
alice.events, bob.events = [], []
bob.cmd("dial", "sip:alice@127.0.0.1")
rang = alice.wait_event("CALL_INCOMING", 10)
bob.cmd("hangup")
time.sleep(1)
print("  alice rang" if rang else "  alice did not ring")
sys.exit(0 if rang else 1)
' "$W"
}

echo "== 1. keepalives on, 130 s"
instance alice 5071 udp 127.0.0.1:5090
instance bob 5072 tcp 127.0.0.1
sleep 130
grep -h "t48keepalive" $W/alice/log $W/bob/log | sed 's/^/  log: /'
for who in alice bob; do
    grep "OPTIONS $who from" $W/kamailio.log | sed 's/.* at //' > $W/$who.times || true
    echo "  $who OPTIONS at $(tr '\n' ' ' < $W/$who.times)"
done
python3 - $W <<'EOF' || FAILS=$((FAILS + 1))
import sys
w = sys.argv[1]
t = [int(x) for x in open(w + "/alice.times").read().split()]
gaps = [b - a for a, b in zip(t, t[1:])]
print(f"  alice gaps {gaps}")
ok = len(t) >= 4 and all(23 <= g <= 30 for g in gaps)
print(("PASS" if ok else "FAIL") + " alice keepalive every 23 to 29 s")
b = open(w + "/bob.times").read().split()
okb = len(b) == 1
print(("PASS" if okb else "FAIL") + f" bob keepalive once in 130 s ({len(b)})")
sys.exit(0 if ok and okb else 1)
EOF
if grep -q "dropped" $W/nat.log; then fail "binding kept"; else pass "binding kept"; fi
if bob_calls_alice; then pass "call reaches alice behind the NAT"; else fail "call reaches alice behind the NAT"; fi

echo "== 2. router restarts"
kill -USR1 $NAT
i=0
until grep -q "public address now" $W/alice/log || [ $i -ge 40 ]; do sleep 1; i=$((i + 1)); done
grep -h "public address now" $W/alice/log | sed 's/^/  log: /'
sleep 2
if grep -q "public address now" $W/alice/log; then pass "new binding noticed in $i s"; else fail "new binding noticed"; fi
if bob_calls_alice; then pass "call reaches alice after the restart"; else fail "call reaches alice after the restart"; fi

echo "== 3. keepalive=off"
kill $PID_alice; wait $PID_alice 2>/dev/null || true
instance alice 5071 udp 127.0.0.1:5090 keepalive=off
sleep 55
grep "dropped (idle" $W/nat.log | sed 's/^/  nat: /'
if grep -q "dropped (idle" $W/nat.log; then pass "binding dropped when idle"; else fail "binding dropped when idle"; fi
if bob_calls_alice; then fail "call lost without keepalives"; else pass "call lost without keepalives"; fi

echo "== NAT log"
sed 's/^/  /' $W/nat.log | grep -v "out .*: REGISTER\|INVITE\|ACK\|BYE" | head -30
echo "failures: $FAILS"
[ $FAILS = 0 ]
