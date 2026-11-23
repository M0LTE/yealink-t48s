#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build libre and baresip (both BSD-3-Clause) for the T48S: static, ARMv6,
# musl, soft-float, with only the modules the phone needs (issue #25,
# docs/sip.md). Plain script for now; it becomes Buildroot packages in the
# external tree once issue #16 lands.
#
#   sip/build.sh              ARM build for the phone (needs $T/bin/armcc, made
#                             by native/build-toolchain.sh)
#   TARGET=host sip/build.sh  the same versions and modules for this PC, used
#                             by sip/test/call-test.sh
#   TLS=1 sip/build.sh        OpenSSL instead of mbedTLS, adding SIP over TLS
#
# libre needs a crypto library even without TLS (MD5 for digest login, SHA-1
# and HMAC for STUN). The default build uses mbedTLS for that alone; libre
# only does TLS with OpenSSL.
#
# Needs: clang, lld, llvm-ar, cmake, make, curl, patch (sip/Dockerfile has them all).
# Output: $OUT/bin/baresip, sizes printed at the end.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
T=${T:-$HOME/src-build/t48s}
TARGET=${TARGET:-arm}
TLS=${TLS:-0}
JOBS=${JOBS:-$(nproc)}
SUFFIX=; [ "$TLS" = 1 ] && SUFFIX=-tls
OUT=${OUT:-$T/sip-$TARGET$SUFFIX}
DL=${DL:-$T/dl}

RE=re-4.13.0
BARESIP=baresip-4.13.0
G722=libg722-1.2.8
OPENSSL=openssl-3.5.9
MBEDTLS=mbedtls-3.6.8

# Modules compiled into the binary. account: the accounts file; menu: the call
# commands (dial, accept, hangup) that t48ctrl runs; g711, libg722:
# codecs; auresamp: 16 kHz device audio to and from 8 kHz G.711; aufile,
# ausine: file and tone audio for tests; stdio: a console for testing by
# hand. Ours, from sip/modules: t48audio (the call's audio, to t48-audiod)
# and t48ctrl (JSON control for the shell on a root-only Unix socket; it
# replaces baresip's ctrl_tcp, which any local program could reach).
MODULES="account;menu;g711;libg722;auresamp;aufile;ausine;stdio"
APP_MODULES="t48audio;t48ctrl"

LLVM_AR=$(command -v llvm-ar || command -v llvm-ar-21)
LLVM_RANLIB=$(command -v llvm-ranlib || command -v llvm-ranlib-21)
LLVM_STRIP=$(command -v llvm-strip || command -v llvm-strip-21)

mkdir -p "$DL" "$OUT"
fetch() { # name url sha256
    [ -f "$DL/$1" ] || curl -fsSL -o "$DL/$1" "$2"
    echo "$3  $DL/$1" | sha256sum -c -
}
fetch $RE.tar.gz https://github.com/baresip/re/archive/refs/tags/v4.13.0.tar.gz \
    3e62b66cb833f22561ec3351647efb3c488766978d72f56de1039a7e5ddf70a9
fetch $BARESIP.tar.gz https://github.com/baresip/baresip/archive/refs/tags/v4.13.0.tar.gz \
    688dbb92844340c2dd8dab7d93aaebccb3aa433af9215bb099ca598fafc0e0a2
fetch $G722.tar.gz https://github.com/sippy/libg722/archive/refs/tags/v1.2.8.tar.gz \
    953bf2ead9b0b0dde32fcbc022a2cf5029155405737eba203499e30b18610706
if [ "$TLS" != 1 ]; then
    fetch $MBEDTLS.tar.bz2 https://github.com/Mbed-TLS/mbedtls/releases/download/$MBEDTLS/$MBEDTLS.tar.bz2 \
        bc54b818e87f49c0b3281dd85d24adaede0e63b4e822e3fbd49c005a38b9595b
else
    fetch $OPENSSL.tar.gz https://github.com/openssl/openssl/releases/download/$OPENSSL/$OPENSSL.tar.gz \
        603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
fi

cd "$OUT"
P="$OUT/prefix"
mkdir -p "$P/lib" "$P/include" src

if [ "$TARGET" = arm ]; then
    [ -x "$T/bin/armcc" ] || { echo "no $T/bin/armcc: run native/build-toolchain.sh first" >&2; exit 1; }
    CC="$T/bin/armcc"
    cat > toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_C_FLAGS_INIT -Wno-unused-command-line-argument)
set(CMAKE_AR $LLVM_AR)
set(CMAKE_RANLIB $LLVM_RANLIB)
set(CMAKE_FIND_ROOT_PATH $T/sysroot $P)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
    XCMAKE="-DCMAKE_TOOLCHAIN_FILE=$OUT/toolchain.cmake"
else
    CC=${CC:-cc}
    XCMAKE="-DCMAKE_C_COMPILER=$CC"
fi
COMMON="-DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_PREFIX_PATH=$P"

# libg722 (the G.722 codec baresip's libg722 module wraps): two C files
rm -rf src/$G722 && tar -C src -xf "$DL/$G722.tar.gz"
(cd src/$G722 && for f in g722_encode g722_decode; do $CC -Os -w -c $f.c -o $f.o; done &&
    rm -f "$P/lib/libg722.a" && $LLVM_AR rcs "$P/lib/libg722.a" g722_encode.o g722_decode.o &&
    cp g722.h g722_codec.h g722_encoder.h g722_decoder.h g722_common.h "$P/include/")

# Crypto: mbedTLS (hashes only), or OpenSSL for TLS=1
if [ "$TLS" = 1 ]; then
    rm -rf src/$OPENSSL && tar -C src -xf "$DL/$OPENSSL.tar.gz"
    if [ "$TARGET" = arm ]; then OSSLT=linux-armv4; else OSSLT=linux-x86_64; fi
    (cd src/$OPENSSL && CC="$CC" AR=$LLVM_AR RANLIB=$LLVM_RANLIB ./Configure $OSSLT \
        --prefix="$P" --libdir=lib no-shared no-tests no-apps no-docs no-engine \
        no-dso no-legacy no-quic no-comp no-ssl3 no-dtls1 no-idea no-mdc2 no-rc5 \
        no-camellia no-seed no-sm2 no-sm3 no-sm4 no-whirlpool no-aria no-siphash \
        no-cms no-ts no-ocsp no-srp no-psk no-gost -Os >../openssl.log &&
        make -j"$JOBS" build_libs >>../openssl.log && make install_dev >>../openssl.log)
    CRYPTO="-DUSE_OPENSSL=ON -DUSE_MBEDTLS=OFF -DOPENSSL_ROOT_DIR=$P -DOPENSSL_USE_STATIC_LIBS=ON"
else
    rm -rf src/$MBEDTLS build-mbedtls && tar -C src -xf "$DL/$MBEDTLS.tar.bz2"
    cmake -S src/$MBEDTLS -B build-mbedtls $XCMAKE $COMMON -DENABLE_PROGRAMS=OFF \
        -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF >build-mbedtls.log
    cmake --build build-mbedtls -j"$JOBS" --target mbedcrypto >>build-mbedtls.log
    mkdir -p "$P/include" && cp -r src/$MBEDTLS/include/mbedtls src/$MBEDTLS/include/psa "$P/include/"
    cp build-mbedtls/library/libmbedcrypto.a "$P/lib/"
    # libre's CMake finds mbedTLS but neither adds its include path nor
    # links it, hence CFLAGS and CMAKE_C_STANDARD_LIBRARIES
    CRYPTO="-DUSE_OPENSSL=OFF -DUSE_MBEDTLS=ON -DMBEDTLS_INCLUDE_DIR=$P/include
        -DMBEDTLS_LIBRARY=$P/lib/libmbedcrypto.a -DCMAKE_C_STANDARD_LIBRARIES=$P/lib/libmbedcrypto.a"
    export CFLAGS="-I$P/include"
fi

# libre
rm -rf src/$RE build-re && tar -C src -xf "$DL/$RE.tar.gz"
for p in "$HERE"/patches/re-*.patch; do patch -s -d src/$RE -p1 < "$p"; done
cmake -S src/$RE -B build-re $XCMAKE $COMMON $CRYPTO \
    -DLIBRE_BUILD_SHARED=OFF -DLIBRE_BUILD_STATIC=ON \
    -DUSE_BFCP=OFF -DUSE_PCP=OFF -DUSE_RTMP=OFF -DUSE_TRACE=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON -DCMAKE_DISABLE_FIND_PACKAGE_Backtrace=ON >build-re.log
cmake --build build-re -j"$JOBS" >>build-re.log
cmake --install build-re >>build-re.log

# baresip, with our modules from sip/modules
rm -rf src/$BARESIP build-baresip && tar -C src -xf "$DL/$BARESIP.tar.gz"
for p in "$HERE"/patches/baresip-*.patch; do [ -f "$p" ] && patch -s -d src/$BARESIP -p1 < "$p"; done
cmake -S src/$BARESIP -B build-baresip $XCMAKE $COMMON $CRYPTO \
    -DSTATIC=ON -DMODULES="$MODULES" \
    -DAPP_MODULES_DIR="$HERE/modules" -DAPP_MODULES="$APP_MODULES" \
    -DLIBG722_INCLUDE_DIR="$P/include" -DLIBG722_LIBRARY="$P/lib/libg722.a" \
    -DRE_INCLUDE_DIR="$P/include/re" -DRE_LIBRARY="$P/lib/libre.a" \
    -Dre_DIR="$P/lib/cmake/re" >build-baresip.log 2>&1
grep MODULES_DETECTED build-baresip.log || true
cmake --build build-baresip -j"$JOBS" --target baresip_exe >>build-baresip.log

mkdir -p bin
cp build-baresip/baresip bin/baresip
if [ "$TARGET" = arm ]; then $LLVM_STRIP bin/baresip; else strip bin/baresip; fi
mkdir -p share/licences
cp src/$RE/LICENSE share/licences/re.LICENSE
cp src/$BARESIP/LICENSE share/licences/baresip.LICENSE
cp src/$G722/LICENSE share/licences/libg722.LICENSE
[ "$TLS" = 1 ] || cp src/$MBEDTLS/LICENSE share/licences/mbedtls.LICENSE
if [ "$TLS" = 1 ]; then cp src/$OPENSSL/LICENSE.txt share/licences/openssl.LICENSE; fi
file bin/baresip
ls -l bin/baresip
