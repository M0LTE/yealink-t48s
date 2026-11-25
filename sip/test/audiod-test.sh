#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# End to end, off the phone and silent: a sound file is the "microphone" of
# one t48-audiod, goes through baresip's t48audio module, over SIP and RTP
# through a throwaway Kamailio to a second baresip and its own t48-audiod,
# whose "loudspeaker" is a file; and the same the other way (issue #24,
# docs/audiod.md). audiod_call.py plays the shell: routes, ringing, tones,
# mute, over the control socket.
#
#   sip/test/audiod-test.sh      in the sip/Dockerfile container, after
#                                TARGET=host sip/build.sh and make -C audiod
#
# With an ARM baresip, an ARM t48-audiod (make -C audiod CC=$T/bin/armcc
# O=build-arm) and qemu-arm, one more call runs alice's side as the phone's
# own binaries.
set -e
SIP=$(cd "$(dirname "$0")/.." && pwd)
AUDIOD=$SIP/../audiod
T=${T:-$HOME/src-build/t48s}
HOST_BARESIP=${HOST_BARESIP:-$T/sip-host/bin/baresip}
ARM_BARESIP=${ARM_BARESIP:-$T/sip-arm/bin/baresip}
W=/tmp/t48-audiod-test
rm -rf $W && mkdir -p $W
FAILS=0

PIDS=
cleanup() { kill $PIDS $KAM 2>/dev/null || true; }
trap cleanup EXIT

echo "== registrar"
sed "s|/tmp/t48-sip-test|$W|" "$SIP/test/kamailio.cfg" > $W/kamailio.cfg
kamailio -f $W/kamailio.cfg -DD -E -Y $W -w $W >$W/kamailio.log 2>&1 &
KAM=$!
sleep 1

python3 "$SIP/test/audiod_call.py" make-wavs $W

# side NAME PORT TRANSPORT PASSWORD CODECS ARM(0|1) AUDIOD-ARGS...
# One phone: a t48-audiod on the file backend and a baresip using it
side() {
    who=$1 port=$2 transport=$3 password=$4 codecs=$5 arm=$6
    shift 6
    d=$W/$who
    rm -rf $d && mkdir -p $d
    if [ "$arm" = 1 ]; then
        AUDIOD_BIN="qemu-arm $AUDIOD/build-arm/t48-audiod"; BARESIP_BIN="qemu-arm $ARM_BARESIP"
    else
        AUDIOD_BIN=$AUDIOD/build/t48-audiod; BARESIP_BIN=$HOST_BARESIP
    fi
    $AUDIOD_BIN --run-dir $d/audio --spk $d/spk.wav "$@" 2>$d/audiod.log &
    PIDS="$PIDS $!"
    sed -e "s|^sip_listen.*|sip_listen\t\t127.0.0.1:$port|" \
        -e "s|^rtp_ports.*|rtp_ports\t\t$(( (port - 5070) * 1000 + 20000 ))-$(( (port - 5070) * 1000 + 20099 ))|" \
        "$SIP/config/config" > $d/config.in
    # baresip skips loopback addresses unless named here
    printf 'net_interface\t\t127.0.0.1\nt48audio_socket\t\t%s\n' $d/audio/call.sock >> $d/config.in
    # the codec under test is the first one offered
    case "$codecs" in
        g711) sed -i '/^module\t*\(opus\|libg722\).so/d' $d/config.in ;;
        g722) sed -i '/^module\t*opus.so/d' $d/config.in ;;
    esac
    printf 'user=%s\npassword=%s\ndomain=127.0.0.1\ntransport=%s\nregister_seconds=60\n' \
        "$who" "$password" "$transport" > $d/sip
    printf '#!/bin/sh\nexec %s "$@"\n' "$BARESIP_BIN" > $d/baresip
    chmod +x $d/baresip
    T48_SIP_SETTINGS=$d/sip T48_SIP_CONFIG=$d/config.in T48_SIP_RUN=$d/run BARESIP=$d/baresip \
        "$SIP/t48-sip" -c >$d/log 2>&1 &
    PIDS="$PIDS $!"
}

stop() { kill $PIDS 2>/dev/null || true; wait $PIDS 2>/dev/null || true; PIDS=; }

# run NAME SCENARIO CODECS ALICE-ARM BOB-AUDIOD-ARGS...
run() {
    name=$1 scenario=$2 codecs=$3 arm=$4
    shift 4
    echo "== $name"
    side alice 5071 udp test-only-alice "$codecs" "$arm" --mic $W/alice-mic.wav
    side bob 5072 tcp test-only-bob "$codecs" 0 "$@"
    sleep 3
    if python3 "$SIP/test/audiod_call.py" "$scenario" $W "$AUDIOD/build/t48-audioctl"; then
        echo "PASS $name"
    else
        echo "FAIL $name"; FAILS=$((FAILS + 1))
    fi
    stop
    grep -h "call stream closed" $W/alice/audiod.log $W/bob/audiod.log | sed 's/^/  /'
    grep -h "t48audio" $W/alice/log $W/bob/log | sort -u | sed 's/^/  baresip: /' | head -6
    # kept for a look afterwards: what each side played, and the logs
    for s in alice bob; do
        mkdir -p "$W/kept/$scenario-$codecs"
        cp $W/$s/spk.wav "$W/kept/$scenario-$codecs/$s-spk.wav"
        cp $W/$s/audiod.log "$W/kept/$scenario-$codecs/$s-audiod.log"
    done
}

run "opus call, handset both ends" call opus 0 --mic $W/bob-mic.wav
run "g722 call, handset both ends" call g722 0 --mic $W/bob-mic.wav
run "g711 call, handset both ends" call g711 0 --mic $W/bob-mic.wav
run "echo: bob on the handset route in an echoing room" echo-handset g722 0 --room 60,50
run "echo: bob on the speakerphone in the same room" echo-speaker g722 0 --room 60,50
if command -v qemu-arm >/dev/null && [ -x "$ARM_BARESIP" ] && [ -x "$AUDIOD/build-arm/t48-audiod" ]; then
    run "g722 call, alice is the ARM binaries under qemu" call-slow g722 1 --mic $W/bob-mic.wav
    run "opus call, alice is the ARM binaries under qemu" call-slow opus 1 --mic $W/bob-mic.wav
else
    echo "== (ARM call skipped: needs qemu-arm, the ARM baresip and audiod/build-arm)"
fi

echo "failures: $FAILS"
[ $FAILS = 0 ]
