#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Registration back-off (issue #77, docs/sip.md#registration-back-off): builds
# sip/test/regbackoff.c against the host libre from TARGET=host sip/build.sh
# (with sip/patches applied) and runs it. A fake clock: it takes a second.
#
#   sip/test/regbackoff-test.sh     in the sip/Dockerfile container, or
#                                   anywhere with clang and the host build
set -e
SIP=$(cd "$(dirname "$0")/.." && pwd)
T=${T:-$HOME/src-build/t48s}
P=${HOST_PREFIX:-$T/sip-host/prefix}
W=/tmp/t48-regbackoff
mkdir -p $W
# a host build from before #78 has mbedTLS, not OpenSSL, and no re-0002
if [ ! -f "$P/lib/libre.a" ] || [ ! -f "$P/lib/libssl.a" ]; then
    echo "regbackoff-test: no host build with OpenSSL in $P; run TARGET=host sip/build.sh (or set HOST_PREFIX)" >&2
    exit 1
fi

clang -O1 -g -I"$P/include/re" -DHAVE_THREADS -DHAVE_PTHREAD -DHAVE_INTTYPES_H -DHAVE_STDBOOL_H \
    "$SIP/test/regbackoff.c" -o $W/regbackoff \
    -Wl,--wrap=clock_gettime -Wl,--wrap=sendto -Wl,--wrap=rand_u16 \
    "$P/lib/libre.a" "$P/lib/libssl.a" "$P/lib/libcrypto.a" -lpthread -ldl -lm
$W/regbackoff
