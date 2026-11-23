#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The phone, end to end, off the phone and silent (issue #26, docs/phone.md).
# The simulator (t48-sim, which is t48-shell) plays the phone: scripted keys,
# hook and touches. Behind it are a real baresip with our t48ctrl and
# t48audio modules and a real t48-audiod on its null backend, registered as
# 1001 to a throwaway Kamailio on loopback. Two more baresips are the far
# ends, 1002 and 1003, driven by far.py from the scripts' `run` lines.
# Each script in shell/tests/phone/ is one scenario: it checks state as it
# goes and saves pictures into $W/shots.
#
#   sip/test/phone-test.sh [SCRIPT...]   in the sip/Dockerfile container, after
#                                        TARGET=host sip/build.sh, make -C audiod
#                                        and make -C shell all build/apps
set -e
SIP=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$SIP/..
T=${T:-$HOME/src-build/t48s}
BARESIP=${HOST_BARESIP:-$T/sip-host/bin/baresip}
W=${W:-/tmp/t48-phone-test}
rm -rf $W && mkdir -p $W/shots $W/phone-config
FAILS=0

PIDS=
cleanup() { kill $PIDS $KAM 2>/dev/null || true; }
trap cleanup EXIT

echo "== registrar"
sed "s|/tmp/t48-sip-test|$W|" "$SIP/test/kamailio.cfg" > $W/kamailio.cfg
kamailio -f $W/kamailio.cfg -DD -E -Y $W -w $W >$W/kamailio.log 2>&1 &
KAM=$!
sleep 1

# side NAME USER PORT TRANSPORT DISPLAY-NAME SETTINGS-FILE [far]: one baresip, started by t48-sip as on the phone
side() {
    d=$W/$1
    mkdir -p $d
    sed -e "s|^sip_listen.*|sip_listen\t\t127.0.0.1:$3|" \
        -e "s|^rtp_ports.*|rtp_ports\t\t$(( ($3 - 5070) * 1000 + 20000 ))-$(( ($3 - 5070) * 1000 + 20099 ))|" \
        "$SIP/config/config" > $d/config.in
    # baresip skips loopback addresses unless named here
    printf 'net_interface\t\t127.0.0.1\nt48audio_socket\t\t%s\n' $d/audio/call.sock >> $d/config.in
    # A far end sends a tone and writes what it hears to a file. (t48audio carries one call
    # at a time, and a far end that is being transferred has two for a moment.)
    [ "$7" = far ] && sed -i -e "s|^audio_source.*|audio_source\t\tausine,440|" \
        -e "s|^audio_player.*|audio_player\t\taufile,$d/heard.wav|" $d/config.in
    printf 'user=%s\npassword=test-only-%s\ndomain=127.0.0.1\ntransport=%s\ndisplay_name=%s\nregister_seconds=60\n' \
        "$2" "$2" "$4" "$5" > "$6"
    T48_SIP_SETTINGS=$6 T48_SIP_CONFIG=$d/config.in T48_SIP_RUN=$d/run BARESIP=$BARESIP \
        "$SIP/t48-sip" -c >$d/log 2>&1 &
    PIDS="$PIDS $!"
}

# the phone: its audio service (null backend: no sound device is touched), then baresip.
# The settings file is the one the shell reads too, as /data/config/sip is on the phone.
mkdir -p $W/phone
"$ROOT/audiod/build/t48-audiod" --run-dir $W/phone/audio 2>$W/audiod.log &
PIDS="$PIDS $!"
side phone 1001 5071 udp "Desk phone" $W/phone-config/sip
side bob 1002 5072 tcp "Bob Example" $W/bob.sip far
side carol 1003 5073 udp "Carol Example" $W/carol.sip far

export FAR="python3 $SIP/test/far.py"
export PHONE=$W/phone/run/ctrl.sock BOB=$W/bob/run/ctrl.sock CAROL=$W/carol/run/ctrl.sock
export BOBLOG=$W/bob.events CAROLLOG=$W/carol.events
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -S $PHONE ] && [ -S $BOB ] && [ -S $CAROL ] && break
    sleep 0.5
done
$FAR $BOB log $BOBLOG & PIDS="$PIDS $!"
$FAR $CAROL log $CAROLLOG & PIDS="$PIDS $!"
sleep 2

SCRIPTS=${*:-$ROOT/shell/tests/phone/*.script}
cd $W
for s in $SCRIPTS; do
    name=$(basename $s .script)
    echo "== $name"
    # each scenario starts with nothing remembered, unless it says it follows on from the one before
    grep -q '^# keep-state' $s || rm -rf $W/phone-state $W/phone-config/audio
    : > $BOBLOG; : > $CAROLLOG
    if "$ROOT/shell/build/t48-sim" --apps "$ROOT/shell/build/apps" --data $W/appdata --config $W/phone-config \
            --state $W/phone-state --sip $PHONE --audio $W/phone/audio/ctl.sock --script $s >$W/$name.log 2>&1; then
        echo "PASS $name"
    else
        echo "FAIL $name"; FAILS=$((FAILS + 1))
        grep "FAILED\|far.py" $W/$name.log | sed 's/^/  /'
    fi
    # leave nothing behind for the next one
    if ! grep -q '^# leave-call' $s; then
        for c in $PHONE $BOB $CAROL; do $FAR $c cmd hangupall dir=all >/dev/null 2>&1 || true; done
        "$ROOT/audiod/build/t48-audioctl" -s $W/phone/audio/ctl.sock route none tone off ring 0 >/dev/null 2>&1 || true
        sleep 0.5
    fi
done

echo "failures: $FAILS"
[ $FAILS = 0 ]
