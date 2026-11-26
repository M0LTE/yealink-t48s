#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# SIP interoperability (issue #77, docs/sip.md#interoperability): calls
# through the throwaway Kamailio of call-test.sh in the shapes a provider's
# network takes, and with a scripted far end (uac.py) where a second baresip
# cannot play the part.
#
#   1. Record-Route with ;lr (loose routing), two entries as the proxy moves
#      the call from UDP to TCP: hold, resume and BYE go through the proxy
#   2. a transparent proxy (no Record-Route): the same go straight between
#      the phones
#   3. topology hiding (topos), as a B2BUA: each side sees only the proxy
#   4. 100rel (RFC 3262): reliable 180 and PRACK between two phones, a caller
#      that requires it, early media in a reliable 183, a phone that
#      requires it
#   5. session timers (RFC 4028, sip/modules/t48timer): either side the
#      refresher, by UPDATE (one answered 100 then 200) or re-INVITE; a
#      caller that requires timers; a caller without them; one asking for
#      too little (422); the setting off, as by default
#   6. a proxy that ends calls nobody refreshes (Kamailio's sst): calls the
#      phones refresh stay up, one nobody refreshes is ended
#
#   sip/test/interop-test.sh    in the sip/Dockerfile container, after
#                               TARGET=host sip/build.sh. About 10 minutes.
set -e
SIP=$(cd "$(dirname "$0")/.." && pwd)
T=${T:-$HOME/src-build/t48s}
HOST_BARESIP=${HOST_BARESIP:-$T/sip-host/bin/baresip}
W=/tmp/t48-sip-test
rm -rf $W/* && mkdir -p $W
FAILS=0

PIDS=
KAM=
cleanup() { kill $PIDS $KAM 2>/dev/null || true; wait 2>/dev/null || true; }
trap cleanup EXIT

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }

# proxy [DEFINE...]: Kamailio with those variants of kamailio.cfg
proxy() {
    [ -n "$KAM" ] && { kill $KAM; wait $KAM 2>/dev/null || true; }
    defs=
    for d in UAC_TRUSTED "$@"; do defs="$defs -A $d"; done
    kamailio -f "$SIP/test/kamailio.cfg" $defs -DD -E -Y $W -w $W >$W/kamailio.log 2>&1 &
    KAM=$!
    sleep 1
}

# instance NAME PORT TRANSPORT [SETTING...]
instance() {
    d=$W/$1
    rm -rf $d && mkdir -p $d
    sed -e "s|^sip_listen.*|sip_listen\t\t127.0.0.1:$2|" \
        -e "s|^rtp_ports.*|rtp_ports\t\t$(( ($2 - 5070) * 1000 + 20000 ))-$(( ($2 - 5070) * 1000 + 20099 ))|" \
        -e "s|^audio_source.*|audio_source\t\tausine,440|" \
        -e "s|^audio_player.*|audio_player\t\taufile,/dev/null|" \
        "$SIP/config/config" > $d/config.in
    printf 'net_interface\t\t127.0.0.1\n' >> $d/config.in
    printf 'user=%s\npassword=test-only-%s\ndomain=127.0.0.1\ntransport=%s\nregister_seconds=600\nmwi=off\n' \
        "$1" "$1" "$3" > $d/sip
    shift 3
    for s in "$@"; do printf '%s\n' "$s" >> $d/sip; done
    T48_SIP_SETTINGS=$d/sip T48_SIP_CONFIG=$d/config.in T48_SIP_RUN=$d/run BARESIP=$HOST_BARESIP \
        "$SIP/t48-sip" -s >$d/log 2>&1 &
    PIDS="$PIDS $!"
}

# stop NAME: end the phones, keep their logs and the proxy's as NAME-*.log
stop() {
    kill $PIDS 2>/dev/null || true; wait $PIDS 2>/dev/null || true; PIDS=
    for d in $W/*/log; do [ -f "$d" ] && mv "$d" "$W/$1-$(basename $(dirname $d)).log"; done
    cp $W/kamailio.log $W/$1-kamailio.log
}

# talk [hold]: alice calls bob, bob answers; with hold, alice holds and
# resumes; alice hangs up. Exit 0 if every step happened.
talk() {
    PYTHONPATH="$SIP/test" python3 - "$W" "$1" <<'EOF'
import sys, time
from ctrl import Ctrl
w, hold = sys.argv[1], sys.argv[2] == "hold"
alice, bob = Ctrl("alice", w + "/alice/run/ctrl.sock"), Ctrl("bob", w + "/bob/run/ctrl.sock")
alice.events, bob.events = [], []
alice.cmd("dial", "sip:bob@127.0.0.1")
if not bob.wait_event("CALL_INCOMING", 10):
    print("  bob did not ring"); sys.exit(1)
bob.cmd("accept")
if not (alice.wait_event("CALL_ESTABLISHED", 10) and bob.wait_event("CALL_ESTABLISHED", 5)):
    print("  not established"); sys.exit(1)
time.sleep(1)
if hold:
    alice.cmd("hold"); time.sleep(1)
    alice.cmd("resume"); time.sleep(1)
alice.cmd("hangup")
ok = bob.wait_event("CALL_CLOSED", 10) is not None
print("  bob saw the call close" if ok else "  bob did not see the call close")
sys.exit(0 if ok else 1)
EOF
}

# routed METHOD: how many in-dialog requests of METHOD the proxy routed
routed() { grep -c "in-dialog $1 " $W/kamailio.log || true; }

echo "== 1. Record-Route, loose routing"
proxy
instance alice 5071 udp
instance bob 5072 tcp
sleep 2
check "call with hold and resume" "talk hold"
echo "  proxy routed: INVITE $(routed INVITE), BYE $(routed BYE)"
check "re-INVITEs and BYE through the proxy" '[ "$(routed INVITE)" -ge 2 ] && [ "$(routed BYE)" -ge 1 ]'
# the 200 alice got for her INVITE: the proxy's two entries, UDP then TCP
rr=$(awk '/^SIP\/2.0 200/ { m = 1; n = 0 } m && /^Record-Route:.*;lr/ { n++ } m && /^CSeq:.*INVITE/ { ok = 1 }
          m && /^\r?$/ { if (ok) { print n; exit } m = 0; ok = 0 }' $W/alice/log)
echo "  Record-Route entries with ;lr in the 200 alice got: $rr"
check "two Record-Route entries with ;lr" '[ "$rr" = 2 ]'
stop rr

# Over TCP the phone's Contact is its end of the connection to the server,
# so another phone cannot open one to it: a server on TCP or TLS has to
# Record-Route, and every one does. Hence UDP here.
echo "== 2. transparent proxy (no Record-Route), UDP"
proxy NO_RR
instance alice 5071 udp
instance bob 5072 udp
sleep 2
check "call with hold and resume" "talk hold"
echo "  proxy routed: INVITE $(routed INVITE), BYE $(routed BYE)"
check "re-INVITEs and BYE straight between the phones" '[ "$(routed INVITE)" = 0 ] && [ "$(routed BYE)" = 0 ]'
stop norr

echo "== 3. topology hiding (topos)"
proxy WITH_TOPOS
instance alice 5071 udp
instance bob 5072 tcp
sleep 2
check "call with hold and resume" "talk hold"
stop topos

# uac ARGS...: the scripted far end (uac.py), its lines kept in $W/uac.out
uac() { python3 "$SIP/test/uac.py" "$@" | tee $W/uac.out | sed 's/^/  uac: /'; }
said() { grep -q "$1" $W/uac.out; }

# alice_calls_uac SECONDS: alice dials uac.py (which must be answering on
# 5080), stays SECONDS, hangs up. Exit 0 if the call was established.
alice_calls_uac() {
    PYTHONPATH="$SIP/test" python3 -c '
import sys, time
from ctrl import Ctrl
a = Ctrl("alice", sys.argv[1]); a.events = []
a.cmd("dial", "sip:uac@127.0.0.1")
ok = a.wait_event("CALL_ESTABLISHED", 10)
end = time.time() + float(sys.argv[2])
while ok and time.time() < end:
    if a.wait_event("CALL_CLOSED", end - time.time()):
        ok = None
a.cmd("hangup")
sys.exit(0 if ok else 1)' $W/alice/run/ctrl.sock "$1" >/dev/null
}

echo "== 4. 100rel"
proxy
instance alice 5071 udp 100rel=on
instance bob 5072 udp 100rel=on
sleep 2
check "both on: call (bob answers before the PRACK is in)" "talk"
check "both on: 180 sent reliably and PRACKed" 'grep -q "^PRACK " $W/alice/log'
stop 100rel-both
instance bob 5072 udp
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 hdr=Supported:100rel hdr=Require:100rel answer-after=$W/bob/run/ctrl.sock
check "caller requires 100rel, phone off (default): 420" 'said "got 420"'
stop 100rel-required-off
instance bob 5072 udp 100rel=on
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 hdr=Supported:100rel hdr=Require:100rel answer-after=$W/bob/run/ctrl.sock
check "caller requires 100rel, phone on: reliable 180, answered" 'said "got 180 rel" && said "prack 200" && said "got 200 "'
stop 100rel-required-on
for mode in off on; do
    instance alice 5071 udp 100rel=$mode
    sleep 2
    uac answer 5080 rel100 hold=4 >/dev/null & UPID=$!
    sleep 0.5
    alice_calls_uac 1 && est=1 || est=0
    wait $UPID || true
    if [ $mode = on ]; then
        check "callee sends early media in a reliable 183, phone on: PRACK, established" \
            '[ $est = 1 ] && grep -q "^PRACK " $W/alice/log'
    else
        check "callee with early media, phone off: plain 183, established" '[ $est = 1 ]'
    fi
    stop 100rel-early-media-$mode
done
instance bob 5072 udp 100rel=required
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 answer-after=$W/bob/run/ctrl.sock
check "phone requires 100rel, caller without: 421" 'said "got 421"'
stop 100rel-phone-requires

echo "== 5. session timers"
proxy
instance bob 5072 udp session_timer=1800
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 hdr=Supported:timer hdr=Session-Expires:90 answer-after=$W/bob/run/ctrl.sock \
    refresh=update refresh=reinvite refresh=reinvite-nosdp hold=8
check "caller asks for 90 s: answered with 90, caller refreshes" 'said "got 200 se=90;refresher=uac require=timer"'
check "refresh by UPDATE" 'said "refresh update 200"'
check "refresh by re-INVITE" 'said "refresh reinvite 200"'
check "refresh by re-INVITE without SDP" 'said "refresh reinvite-nosdp 200"'
check "call still up after the refreshes" 'said "bye 200"'
stop timer-caller-refreshes
instance bob 5072 udp session_timer=1800
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 hdr=Supported:timer hdr=Require:timer hdr=Session-Expires:90 \
    answer-after=$W/bob/run/ctrl.sock
check "caller requires timer: answered" 'said "got 200 se=90;refresher=uac"'
stop timer-required
instance bob 5072 udp session_timer=1800
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 hdr=Supported:timer "hdr=Session-Expires:90;refresher=uas" \
    answer-after=$W/bob/run/ctrl.sock hold=50
check "caller makes the phone refresher: UPDATE within 50 s" 'said "got UPDATE se=90;refresher=uac"'
stop timer-phone-refreshes
instance bob 5072 udp session_timer=1800
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 hdr=Supported:timer "hdr=Session-Expires:90;refresher=uas" no-update \
    answer-after=$W/bob/run/ctrl.sock hold=50
check "the same, caller without UPDATE: re-INVITE within 50 s" 'said "got INVITE sdp"'
stop timer-phone-reinvites
instance bob 5072 udp session_timer=1800
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 answer-after=$W/bob/run/ctrl.sock
check "caller without timers: answered without one" 'said "got 200 se=- "'
stop timer-caller-without
instance alice 5071 udp session_timer=1800
sleep 2
uac answer 5080 "hdr=Session-Expires:90;refresher=uac" hdr=Require:timer slow-update hold=105 >/dev/null & UPID=$!
sleep 0.5
alice_calls_uac 100 || true
wait $UPID || true
sed 's/^/  uac: /' $W/uac.out
check "phone calls: asks for 1800 s" 'said "got INVITE se=1800 supported=.*timer"'
check "callee makes the phone refresher at 90 s, answers UPDATE with 100 then 200: two UPDATEs in 100 s" \
    '[ "$(grep -c "got UPDATE se=90;refresher=uac" $W/uac.out)" -ge 2 ]'
stop timer-phone-calls
instance bob 5072 udp
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 hdr=Supported:timer hdr=Require:timer hdr=Session-Expires:90 \
    answer-after=$W/bob/run/ctrl.sock
check "session timer off (default), caller requires timer: 420" 'said "got 420"'
stop timer-off
instance bob 5072 udp
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 hdr=Supported:timer hdr=Session-Expires:90 answer-after=$W/bob/run/ctrl.sock
check "session timer off (default), caller asks for one: answered without" 'said "got 200 se=- "'
stop timer-off-offered
instance bob 5072 udp session_timer=1800
sleep 2
uac call sip:bob@127.0.0.1:5060 5080 hdr=Supported:timer hdr=Session-Expires:60 answer-after=$W/bob/run/ctrl.sock
check "caller asks for 60 s: 422 with Min-SE: 90" 'said "got 422" && grep -q "^Min-SE: 90" $W/bob/log'
stop timer-too-small

echo "== 6. a proxy that ends calls without refreshes (sst, 90 s)"
proxy WITH_SST
instance alice 5071 udp session_timer=90
instance bob 5072 udp
sleep 2
PYTHONPATH="$SIP/test" python3 -c '
import sys, time
from ctrl import Ctrl
a, b = Ctrl("alice", sys.argv[1] + "/alice/run/ctrl.sock"), Ctrl("bob", sys.argv[1] + "/bob/run/ctrl.sock")
a.events, b.events = [], []
a.cmd("dial", "sip:bob@127.0.0.1")
b.wait_event("CALL_INCOMING", 10); b.cmd("accept")
if not a.wait_event("CALL_ESTABLISHED", 10):
    print("  not established"); sys.exit(1)
t = time.time()
e = b.wait_event("CALL_CLOSED", 130)
print("  call ended by the network after %.0f s" % (time.time() - t) if e else "  call up after 130 s")
a.cmd("hangup")
sys.exit(1 if e else 0)' $W && up=1 || up=0
echo "  proxy routed: UPDATE $(routed UPDATE)"
check "phones refresh: call still up after 130 s" '[ $up = 1 ] && [ "$(routed UPDATE)" -ge 2 ]'
stop sst-refreshed
instance alice 5071 udp session_timer=90
sleep 2
uac answer 5080 "hdr=Session-Expires:90;refresher=uas" hdr=Require:timer hold=130 >/dev/null & UPID=$!
sleep 0.5
t0=$(date +%s)
alice_calls_uac 130 && up=1 || up=0
echo "  call lasted $(( $(date +%s) - t0 )) s"
wait $UPID || true
check "far end never refreshes: the proxy ends the call" '[ $up = 0 ]'
stop sst-unrefreshed

echo "failures: $FAILS"
[ $FAILS = 0 ]
