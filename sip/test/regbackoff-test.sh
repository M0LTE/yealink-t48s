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
[ -f "$P/lib/libre.a" ] || { echo "no $P/lib/libre.a: run TARGET=host sip/build.sh first" >&2; exit 1; }

clang -O1 -g -I"$P/include/re" -DHAVE_THREADS -DHAVE_PTHREAD -DHAVE_INTTYPES_H -DHAVE_STDBOOL_H \
    "$SIP/test/regbackoff.c" -o $W/regbackoff \
    -Wl,--wrap=clock_gettime -Wl,--wrap=sendto -Wl,--wrap=rand_u16 \
    "$P/lib/libre.a" "$P/lib/libssl.a" "$P/lib/libcrypto.a" -lpthread -ldl -lm
$W/regbackoff
