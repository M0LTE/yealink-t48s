#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# SIP over TLS and SRTP (issue #78, docs/sip.md#sip-over-tls). The test
# Kamailio listens for TLS on 5061 with a certificate for sip.test from a
# test CA made here and thrown away afterwards; a DNS server on loopback
# (dns.py) gives _sips._tcp.sip.test and sip.test. alice and bob run through
# t48-sip as on the phone, with the test CA as a root the owner added.
#
#   1. TLS registration found by SRV, no port given; and with a port
#   2. no added root: the certificate is refused; a name it was not made for:
#      refused; an expired one: refused
#   3. calls between two baresips over TLS: SRTP off, optional, compulsory
#      and DTLS-SRTP, each pairing that should work and those that should not
#   4. the ARM binary under qemu-arm: TLS registration and an SRTP call
#
#   sip/test/tls-test.sh        in the sip/Dockerfile container, after
#                               sip/build.sh and TARGET=host sip/build.sh.
#                               About 3 minutes.
#
# No key here is a real one: each run makes its own, for 127.0.0.1 only.
set -e
SIP=$(cd "$(dirname "$0")/.." && pwd)
T=${T:-$HOME/src-build/t48s}
HOST_BARESIP=${HOST_BARESIP:-$T/sip-host/bin/baresip}
ARM_BARESIP=${ARM_BARESIP:-$T/sip-arm/bin/baresip}
W=/tmp/t48-sip-test
DNSPORT=5353
rm -rf $W/* && mkdir -p $W
FAILS=0
PIDS=
cleanup() { kill $PIDS $KAM $DNS 2>/dev/null || true; }
trap cleanup EXIT
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }

echo "== a test CA and certificates, made now (test only)"
cd $W
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 \
    -keyout ca.key -out ca.pem -subj "/O=t48 test only/CN=t48 test CA" \
    -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign 2>/dev/null
# server: RSA, as most providers use
leaf() { # NAME DAYS SAN
    openssl req -newkey rsa:2048 -nodes -keyout $1.key -out $1.csr -subj "/CN=$3" 2>/dev/null
    printf 'subjectAltName=DNS:%s\nextendedKeyUsage=serverAuth\n' "$3" > $1.ext
    openssl x509 -req -in $1.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days $2 \
        -extfile $1.ext -out $1.crt 2>/dev/null
}
leaf server 2 sip.test
# an expired one: backdated with -not_before/-not_after
openssl req -newkey rsa:2048 -nodes -keyout expired.key -out expired.csr -subj "/CN=sip.test" 2>/dev/null
openssl x509 -req -in expired.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
    -not_before 20200101000000Z -not_after 20200201000000Z -extfile server.ext -out expired.crt 2>/dev/null
mkdir -p certs && cp ca.pem certs/test-ca.pem
cd - >/dev/null

kam_start() { # CERT KEY
    cat > $W/tls.cfg <<EOF
[server:default]
method = TLSv1.2+
verify_certificate = no
require_certificate = no
certificate = $W/$1
private_key = $W/$2

[client:default]
verify_certificate = no
require_certificate = no
EOF
    kamailio -f "$SIP/test/kamailio.cfg" -A WITH_TLS -DD -E -Y $W -w $W >$W/kamailio.log 2>&1 &
    KAM=$!
    sleep 1
}
kam_stop() { kill $KAM 2>/dev/null || true; wait $KAM 2>/dev/null || true; KAM=; }

python3 "$SIP/test/dns.py" $DNSPORT \
    SRV:_sips._tcp.sip.test=0,0,5061,sip.test A:sip.test=127.0.0.1 \
    SRV:_sips._tcp.other.test=0,0,5061,other.test A:other.test=127.0.0.1 &
DNS=$!

# instance NAME BINARY PORT DOMAIN SRTP SOURCE PLAYER [CERTS]
instance() {
    d=$W/$1
    mkdir -p $d
    sed -e "s|^sip_listen.*|sip_listen\t\t127.0.0.1:$3|" \
        -e "s|^rtp_ports.*|rtp_ports\t\t$(( ($3 - 5090) * 100 + 30000 ))-$(( ($3 - 5090) * 100 + 30099 ))|" \
        -e "s|^audio_source.*|audio_source\t\t$6|" \
        -e "s|^audio_player.*|audio_player\t\t$7|" \
        "$SIP/config/config" > $d/config.in
    printf 'net_interface\t\t127.0.0.1\ndns_server\t\t127.0.0.1:%s\n' $DNSPORT >> $d/config.in
    printf 'user=%s\npassword=test-only-%s\ndomain=%s\ntransport=tls\nregister_seconds=60\nmwi=off\nsrtp=%s\n' \
        "$1" "$1" "$4" "$5" > $d/sip
    case "$2" in *sip-arm*) BIN="qemu-arm $2" ;; *) BIN=$2 ;; esac
    printf '#!/bin/sh\nexec %s "$@"\n' "$BIN" > $d/baresip
    chmod +x $d/baresip
    T48_SIP_SETTINGS=$d/sip T48_SIP_CONFIG=$d/config.in T48_SIP_RUN=$d/run BARESIP=$d/baresip \
        T48_SIP_CERTS=${8-$W/certs} "$SIP/t48-sip" -c >$d/log 2>&1 &
    PIDS="$PIDS $!"
}
stop() { kill $PIDS 2>/dev/null || true; wait $PIDS 2>/dev/null || true; PIDS=; }
registered() { # NAME: exit 0 if the account is registered
    PYTHONPATH="$SIP/test" python3 -c '
import sys
from ctrl import Ctrl
c = Ctrl("x", sys.argv[1]); c.events = []
sys.exit(0 if "OK" in c.cmd("reginfo")["data"] else 1)' $W/$1/run/ctrl.sock 2>/dev/null
}
# registrations the server took over TLS since the last mark
mark() { M=$(grep -c "(tls): saved" $W/kamailio.log || true); }
saved() { echo $(( $(grep -c "(tls): saved" $W/kamailio.log || true) - M )); }

kam_start server.crt server.key

echo "== 1. TLS registration"
mark
instance alice "$HOST_BARESIP" 5091 sip.test off ausine,440 aufile,$W/alice-heard.wav
instance bob "$HOST_BARESIP" 5093 sip.test:5061 off ausine,1000 aufile,$W/bob-heard.wav
sleep 4
grep -h "t48keepalive.*tls" $W/alice/log | head -1 | sed 's/^/  log: /'
if registered alice; then pass "TLS registration by SRV, no port"; else fail "TLS registration by SRV, no port"; fi
if registered bob; then pass "TLS registration to host:port"; else fail "TLS registration to host:port"; fi
if [ "$(saved)" -ge 2 ]; then pass "the server saw both over TLS"; else fail "the server saw both over TLS"; fi
if grep -q "^sip_cafile.*ca.pem" $W/alice/run/config && grep -q "^sip_transports.*tls" $W/alice/run/config; then
    pass "the added root reaches baresip"; else fail "the added root reaches baresip"; fi
stop

echo "== 2. certificates that must be refused"
mark
instance alice "$HOST_BARESIP" 5091 sip.test off ausine,440 aufile,$W/x.wav $W/none
sleep 4
if ! registered alice && [ "$(saved)" -eq 0 ]; then pass "no added root: refused"; else fail "no added root: refused"; fi
grep -h "tls_verify_handler: err \|tls: verify" $W/alice/log | head -1 | sed 's/^/  log: /'
stop
mark
instance alice "$HOST_BARESIP" 5091 other.test off ausine,440 aufile,$W/x.wav
sleep 4
if ! registered alice && [ "$(saved)" -eq 0 ]; then
    pass "a name the certificate is not for: refused"; else fail "a name the certificate is not for: refused"; fi
grep -h "tls_verify_handler: err \|tls: verify" $W/alice/log | head -1 | sed 's/^/  log: /'
stop
kam_stop
kam_start expired.crt expired.key
mark
instance alice "$HOST_BARESIP" 5091 sip.test off ausine,440 aufile,$W/x.wav
sleep 4
if ! registered alice && [ "$(saved)" -eq 0 ]; then pass "an expired certificate: refused"; else fail "an expired certificate: refused"; fi
grep -h "tls_verify_handler: err \|tls: verify" $W/alice/log | head -1 | sed 's/^/  log: /'
stop
kam_stop
kam_start server.crt server.key

echo "== 3. calls over TLS"
# call NAME ALICE_SRTP BOB_SRTP secure|plain|refused [ALICE_BINARY]
call() {
    echo "-- $1: alice $2, bob $3, expect $4"
    rm -f $W/*.wav
    instance alice "${5:-$HOST_BARESIP}" 5091 sip.test "$2" ausine,440 aufile,$W/alice-heard.wav
    instance bob "$HOST_BARESIP" 5093 sip.test "$3" ausine,1000 aufile,$W/bob-heard.wav
    i=0
    until registered alice && registered bob; do i=$((i + 1)); [ $i -lt 40 ] || break; sleep 0.5; done
    if PYTHONPATH="$SIP/test" python3 "$SIP/test/srtp_call.py" $W "$4"; then pass "$1"; else fail "$1"; fi
    grep -h "SRTP is Enabled\|verified SHA-256\|No common" $W/alice/log $W/bob/log | sed 's/.*\(srtp\|dtls_srtp\|call\): /\1: /' | sort -u | head -3 | sed 's/^/  log: /'
    stop
    mv $W/alice/log $W/$1-alice.log; mv $W/bob/log $W/$1-bob.log
}
call off-off off off plain
call optional-off optional off plain
call off-optional off optional plain
call optional-optional optional optional secure
call compulsory-compulsory compulsory compulsory secure
call compulsory-optional compulsory optional secure
call optional-compulsory optional compulsory refused
call compulsory-off compulsory off refused
call off-compulsory off compulsory refused
call dtls-dtls dtls dtls secure
call dtls-off dtls off refused

if command -v qemu-arm >/dev/null && [ -x "$ARM_BARESIP" ]; then
    echo "== 4. the ARM binary under qemu-arm"
    call arm-compulsory compulsory compulsory secure "$ARM_BARESIP"
fi

echo "== registrar log"
grep -h "REGISTER\|relayed" $W/kamailio.log | sed 's/^.*INFO: <script>: //' | sed 's/:[0-9]* / /' | sort | uniq -c | head -20
echo "failures: $FAILS"
[ $FAILS = 0 ]
