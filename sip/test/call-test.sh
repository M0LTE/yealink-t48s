#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Calls between two baresip instances through a throwaway Kamailio registrar,
# all on loopback (issue #25, docs/sip.md). Both register with a password,
# alice dials bob over t48ctrl, bob answers over t48ctrl, each sends a tone
# and records what it hears, and the recordings are checked.
#
#   sip/test/call-test.sh        in the sip/Dockerfile container, after
#                                sip/build.sh and TARGET=host sip/build.sh
#
# HOST_BARESIP and ARM_BARESIP point at the two builds; the ARM one runs
# under qemu-arm and is skipped if either is missing.
set -e
SIP=$(cd "$(dirname "$0")/.." && pwd)
T=${T:-$HOME/src-build/t48s}
HOST_BARESIP=${HOST_BARESIP:-$T/sip-host/bin/baresip}
ARM_BARESIP=${ARM_BARESIP:-$T/sip-arm/bin/baresip}
W=/tmp/t48-sip-test
rm -rf $W/* && mkdir -p $W
FAILS=0

PIDS=
cleanup() { kill $PIDS $KAM 2>/dev/null || true; }
trap cleanup EXIT

echo "== registrar"
kamailio -f "$SIP/test/kamailio.cfg" -DD -E -Y $W -w $W >$W/kamailio.log 2>&1 &
KAM=$!
sleep 1

# instance NAME BINARY PORT TRANSPORT PASSWORD SOURCE PLAYER CODECS
instance() {
    d=$W/$1
    mkdir -p $d
    sed -e "s|^sip_listen.*|sip_listen\t\t127.0.0.1:$3|" \
        -e "s|^rtp_ports.*|rtp_ports\t\t$(( ($3 - 5070) * 1000 + 20000 ))-$(( ($3 - 5070) * 1000 + 20099 ))|" \
        -e "s|^audio_source.*|audio_source\t\t$6|" \
        -e "s|^audio_player.*|audio_player\t\t$7|" \
        "$SIP/config/config" > $d/config.in
    # baresip skips loopback addresses unless named here
    printf 'net_interface\t\t127.0.0.1\n' >> $d/config.in
    # the codec under test is the first one offered
    case "$8" in
        g711) sed -i '/^module\t*\(opus\|libg722\).so/d' $d/config.in ;;
        g722) sed -i '/^module\t*opus.so/d' $d/config.in ;;
    esac
    printf 'user=%s\npassword=%s\ndomain=127.0.0.1\ntransport=%s\nregister_seconds=60\n' \
        "${1%%-*}" "$5" "$4" > $d/sip
    case "$2" in *sip-arm*) BIN="qemu-arm $2" ;; *) BIN=$2 ;; esac
    # t48-sip runs "$BARESIP -f DIR", so a wrapper carries the qemu prefix
    printf '#!/bin/sh\nexec %s "$@"\n' "$BIN" > $d/baresip
    chmod +x $d/baresip
    T48_SIP_SETTINGS=$d/sip T48_SIP_CONFIG=$d/config.in T48_SIP_RUN=$d/run BARESIP=$d/baresip \
        "$SIP/t48-sip" -c >$d/log 2>&1 &
    PIDS="$PIDS $!"
}

stop() { kill $PIDS 2>/dev/null || true; wait $PIDS 2>/dev/null || true; PIDS=; }

# call NAME ALICE_BINARY CODECS BOB_SOURCE BOB_PLAYER BOB_HEARS ALICE_HEARS
call() {
    echo "== $1"
    rm -f $W/*.wav
    instance alice "$2" 5071 udp test-only-alice ausine,440 aufile,$W/alice-heard.wav "$3"
    instance bob "$HOST_BARESIP" 5072 tcp test-only-bob "$4" "$5" "$3"
    sleep 3
    ok=0
    python3 "$SIP/test/ctrl.py" "$6" "$7" $W && ok=1
    # and the call used the codec under test
    case "$3" in opus) enc=opus ;; g722) enc=G722 ;; g711) enc=PCMU ;; esac
    grep -q "Set audio encoder: $enc " $W/alice/log || { echo "  alice did not encode with $enc"; ok=0; }
    if [ $ok = 1 ]; then echo "PASS $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi
    grep -h "codec\|audio: .*encode\|t48audio" $W/alice/log $W/bob/log | grep -iv debug | sort -u | sed 's/^/  log: /' | head -8
    stop
    mv $W/alice/log $W/$1-alice.log; mv $W/bob/log $W/$1-bob.log
}

call opus "$HOST_BARESIP" opus ausine,1000 aufile,$W/bob-heard.wav 440 1000
call g711 "$HOST_BARESIP" g711 ausine,1000 aufile,$W/bob-heard.wav 440 1000
call g722 "$HOST_BARESIP" g722 ausine,1000 aufile,$W/bob-heard.wav 440 1000
call t48audio-null "$HOST_BARESIP" g722 t48audio,call t48audio,call none silence
if command -v qemu-arm >/dev/null && [ -x "$ARM_BARESIP" ]; then
    call arm-under-qemu "$ARM_BARESIP" g722 ausine,1000 aufile,$W/bob-heard.wav 440 1000
    call opus-arm-under-qemu "$ARM_BARESIP" opus ausine,1000 aufile,$W/bob-heard.wav 440 1000
fi

echo "== wrong password is refused"
instance alice "$HOST_BARESIP" 5071 udp wrong-password ausine,440 aufile,$W/x.wav g711
sleep 3
if grep -q "401 Unauthorized\|403\|Unauthorized" $W/alice/log; then
    grep -h "reg\b\|Unauthorized" $W/alice/log | head -2 | sed 's/^/  log: /'
    echo "PASS wrong password"
else
    echo "FAIL wrong password"; FAILS=$((FAILS + 1))
fi
stop

echo "== registrar log"
grep -h "REGISTER\|relayed" $W/kamailio.log | sed 's/^.*INFO: <script>: /  /' | sort | uniq -c | head -20
echo "failures: $FAILS"
[ $FAILS = 0 ]
